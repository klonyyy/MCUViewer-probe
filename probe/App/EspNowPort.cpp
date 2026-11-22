/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "EspNowPort.hpp"

#include <inttypes.h>
#include <string.h>
#include <unistd.h>

#include <atomic>

#include "EspNowRate.hpp"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace
{
constexpr const char* tag = "espnow_port";
constexpr uint32_t poolSize = 12; /**< Packets the net task has not handled yet; more are dropped. */
constexpr size_t maxPeers = 8;	  /**< Pinned dongles, and those sent to lately: the one unused longest makes room. */

struct Peer
{
	uint8_t mac[6];
	int64_t lastUse;
	bool used;
	bool pinned; /**< A paired dongle: encrypted with lmk, never makes room. */
	uint8_t lmk[ESP_NOW_KEY_LEN];
	EspNowLink::RateLadder ladder;
};

/* The packet in the air: one at a time, so its send callback is known */
enum class TxState : uint8_t
{
	idle,
	inFlight,
	acked,	  /**< service() moves the ladder, then the next packet may go. */
	failed,	  /**< No acknowledgement: service() moves the ladder and sends it again. */
	noBuffer, /**< ESP-NOW was out of buffers: service() sends it again. */
	quiet,	  /**< Failed at the lowest rate: sent again at quietUntil, or once the peer is heard. */
};

EspNowPort::Packet pool[poolSize];
QueueHandle_t freeQueue = nullptr; /* Packet pointers */
QueueHandle_t rxQueue = nullptr;
int wakeFd = -1;
std::atomic<bool> started{false};
std::atomic<bool> running{false};
std::atomic<TxState> txState{TxState::idle};
std::atomic<uint32_t> drops{0};
uint32_t reportedDrops = 0;
uint32_t lost = 0; /**< Packets sent maxResends times without an acknowledgement. */
uint32_t reportedLost = 0;
uint8_t txPacket[EspNowLink::maxPacket];
size_t txLength = 0;
uint8_t txMac[6];
int txResends = 0;
bool txOnce = false; /**< No resends, and the ladder stays: an answer outside a session, or a CLOSE. */
int64_t txStartUs = 0;
std::atomic<int64_t> txEndUs{0};
int64_t quietUntil = 0;
bool quietReported = false; /**< Since the last acknowledgement. */
Peer peers[maxPeers];
uint8_t ownMac[6] = {};
std::atomic<bool> repinNeeded{false}; /**< ESP-NOW started again without the pinned peers. */

void wake()
{
	const uint64_t one = 1;
	write(wakeFd, &one, sizeof(one));
}

/* Wi-Fi task: copy and hand over */
void onReceive(const esp_now_recv_info_t* const info, const uint8_t* const data, const int length)
{
	if (length < static_cast<int>(EspNowLink::headerSize) || length > static_cast<int>(EspNowLink::maxPacket))
		return;
	EspNowPort::Packet* packet = nullptr;
	if (xQueueReceive(freeQueue, &packet, 0) != pdTRUE)
	{
		drops.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	memcpy(packet->mac, info->src_addr, sizeof(packet->mac));
	packet->rssi = static_cast<int8_t>(info->rx_ctrl->rssi);
	packet->channel = static_cast<uint8_t>(info->rx_ctrl->channel);
	packet->length = static_cast<uint16_t>(length);
	memcpy(packet->data, data, length);
	xQueueSend(rxQueue, &packet, 0);
	wake();
}

void onSent(const esp_now_send_info_t* const info, const esp_now_send_status_t status)
{
	txEndUs = esp_timer_get_time();
	txState = status == ESP_NOW_SEND_SUCCESS ? TxState::acked : TxState::failed;
	wake();
}

/* The packet in txPacket, once more */
bool transmit()
{
	txState = TxState::inFlight;
	txStartUs = esp_timer_get_time();
	const esp_err_t result = esp_now_send(txMac, txPacket, txLength);
	if (result == ESP_OK)
		return true;
	/* Out of buffers: the next service() round tries again */
	txState = result == ESP_ERR_ESPNOW_NO_MEM ? TxState::noBuffer : TxState::idle;
	if (result != ESP_ERR_ESPNOW_NO_MEM)
		ESP_LOGW(tag, "esp_now_send(): %s", esp_err_to_name(result));
	return false;
}

uint8_t currentChannel()
{
	uint8_t primary = 0;
	wifi_second_chan_t second;
	return esp_wifi_get_channel(&primary, &second) == ESP_OK ? primary : 0;
}

Peer* findPeer(const uint8_t* const mac)
{
	for (Peer& peer : peers)
		if (peer.used && !memcmp(peer.mac, mac, sizeof(peer.mac)))
			return &peer;
	return nullptr;
}

void applyRate(const Peer& peer)
{
	const uint8_t rung = peer.ladder.getRung();
	const esp_err_t result = EspNowLink::setPeerRate(peer.mac, rung);
	if (result == ESP_OK)
		ESP_LOGI(tag, "sending to " MACSTR " at %s", MAC2STR(peer.mac), EspNowLink::rates[rung].name);
	else
		ESP_LOGW(tag, "rate %s for " MACSTR ": %s", EspNowLink::rates[rung].name, MAC2STR(peer.mac), esp_err_to_name(result));
}

/* The session packet in the air went through or not: its peer's ladder moves */
void learn(const bool acknowledged)
{
	Peer* const peer = txOnce ? nullptr : findPeer(txMac);
	if (peer && peer->ladder.update(acknowledged, esp_timer_get_time()))
		applyRate(*peer);
}

/* Through: the next packet may go */
void settle()
{
	if (txState != TxState::acked)
		return;
	learn(true);
	quietReported = false;
	txState = TxState::idle;
}

/* A session packet failed: at the lowest rate the next try waits, so the transmitter rests while nobody answers */
bool startQuiet()
{
	const Peer* const peer = findPeer(txMac);
	if (!peer || !peer->ladder.atBottom())
		return false;
	const int64_t tryUs = txEndUs.load() - txStartUs;
	const int64_t pauseUs = EspNowLink::quietUs(tryUs);
	quietUntil = esp_timer_get_time() + pauseUs;
	txState = TxState::quiet;
	if (!quietReported)
	{
		ESP_LOGW(tag, "no acknowledgement from " MACSTR " at %s (the try took %lld ms): one try every %lld ms until it is heard", MAC2STR(txMac), EspNowLink::rates[peer->ladder.getRung()].name, static_cast<long long>(tryUs / 1000), static_cast<long long>((tryUs + pauseUs) / 1000));
		quietReported = true;
	}
	return true;
}

bool init()
{
	esp_err_t result = esp_now_init();
	if (result == ESP_OK)
		result = esp_now_register_recv_cb(onReceive);
	if (result == ESP_OK)
		result = esp_now_register_send_cb(onSent);
	if (result != ESP_OK)
	{
		ESP_LOGE(tag, "ESP-NOW does not start: %s", esp_err_to_name(result));
		return false;
	}
	return true;
}

bool isBroadcast(const uint8_t* const mac)
{
	return !memcmp(mac, EspNowLink::broadcastMac, sizeof(EspNowLink::broadcastMac));
}

/* Gives a peer to ESP-NOW as it is: encrypted when pinned, at its rung */
bool applyPeer(const Peer& peer)
{
	esp_now_peer_info_t info = {};
	memcpy(info.peer_addr, peer.mac, sizeof(info.peer_addr));
	info.ifidx = WIFI_IF_STA;
	info.encrypt = peer.pinned;
	if (peer.pinned)
		memcpy(info.lmk, peer.lmk, sizeof(info.lmk));
	if (esp_now_is_peer_exist(peer.mac))
		esp_now_del_peer(peer.mac);
	esp_err_t result = esp_now_add_peer(&info);
	/* Without it ESP-NOW sends at 1 Mb/s, where the acknowledgements get lost; broadcasts stay there, for range */
	if (result == ESP_OK && !isBroadcast(peer.mac))
		result = EspNowLink::setPeerRate(peer.mac, peer.ladder.getRung());
	if (result != ESP_OK)
		ESP_LOGW(tag, "cannot add peer " MACSTR "%s: %s", MAC2STR(peer.mac), peer.pinned ? " (encrypted)" : "", esp_err_to_name(result));
	return result == ESP_OK;
}

/* The slot of mac, or a new one: free, else the one unused longest that is not pinned. A new one starts at the top of the ladder. */
Peer* claimSlot(const uint8_t* const mac)
{
	if (Peer* const peer = findPeer(mac))
		return peer;
	Peer* slot = nullptr;
	for (Peer& peer : peers)
	{
		if (peer.pinned)
			continue;
		if (!peer.used)
		{
			slot = &peer;
			break;
		}
		if (!slot || peer.lastUse < slot->lastUse)
			slot = &peer;
	}
	if (!slot)
		return nullptr;
	if (slot->used && running && esp_now_is_peer_exist(slot->mac))
		esp_now_del_peer(slot->mac);
	*slot = Peer{};
	memcpy(slot->mac, mac, sizeof(slot->mac));
	slot->used = true;
	return slot;
}

/*
 * A peer for sending: added when it is not one (any more). One ESP-NOW forgot keeps its rung, and a pinned one its
 * key.
 */
bool addPeer(const uint8_t* const mac)
{
	const int64_t now = esp_timer_get_time();
	Peer* const existing = findPeer(mac);
	if (existing && esp_now_is_peer_exist(mac))
	{
		existing->lastUse = now;
		return true;
	}
	Peer* const slot = existing ? existing : claimSlot(mac);
	if (!slot)
		return false;
	slot->lastUse = now;
	if (applyPeer(*slot))
		return true;
	if (!slot->pinned)
		slot->used = false;
	return false;
}
}  // namespace

void EspNowPort::start(const int fd)
{
	wakeFd = fd;
	freeQueue = xQueueCreate(poolSize, sizeof(Packet*));
	rxQueue = xQueueCreate(poolSize, sizeof(Packet*));
	for (Packet& packet : pool)
		release(&packet);
	if (!init())
		return;
	started = true;
	running = true;

	esp_read_mac(ownMac, ESP_MAC_WIFI_STA);
	ESP_LOGI(tag, "ESP-NOW listening as " MACSTR, MAC2STR(ownMac));
}

void EspNowPort::suspend()
{
	if (!started || !running.exchange(false))
		return;
	esp_now_deinit();
	/* The packet in the air gets no callback any more */
	txState = TxState::idle;
}

void EspNowPort::resume()
{
	if (!started || running)
		return;
	running = init();
	/* The net task owns the peers: it gives the pinned ones back */
	if (running)
	{
		repinNeeded = true;
		wake();
	}
}

bool EspNowPort::pin(const uint8_t* const mac, const uint8_t* const lmk)
{
	Peer* const slot = claimSlot(mac);
	if (!slot)
	{
		ESP_LOGE(tag, "no room to pin " MACSTR, MAC2STR(mac));
		return false;
	}
	slot->pinned = true;
	memcpy(slot->lmk, lmk, sizeof(slot->lmk));
	slot->lastUse = esp_timer_get_time();
	return !running || applyPeer(*slot);
}

void EspNowPort::unpin(const uint8_t* const mac)
{
	Peer* const slot = findPeer(mac);
	if (!slot || !slot->pinned)
		return;
	/* Added again as an ordinary peer once it is sent to */
	if (running && esp_now_is_peer_exist(mac))
		esp_now_del_peer(mac);
	*slot = Peer{};
}

const uint8_t* EspNowPort::getOwnMac()
{
	return ownMac;
}

EspNowPort::Packet* EspNowPort::receive()
{
	if (!rxQueue)
		return nullptr;
	const uint32_t dropped = drops.load(std::memory_order_relaxed);
	if (dropped != reportedDrops)
	{
		ESP_LOGW(tag, "%" PRIu32 " packets dropped so far: the net task did not keep up", dropped);
		reportedDrops = dropped;
	}
	Packet* packet = nullptr;
	if (xQueueReceive(rxQueue, &packet, 0) != pdTRUE)
		return nullptr;
	/* The peer is there: the packet waiting for it goes now */
	if (txState == TxState::quiet && !memcmp(packet->mac, txMac, sizeof(txMac)))
	{
		quietUntil = 0;
		wake();
	}
	return packet;
}

void EspNowPort::release(Packet* const packet)
{
	xQueueSend(freeQueue, &packet, 0);
}

void EspNowPort::service()
{
	if (lost != reportedLost)
	{
		ESP_LOGW(tag, "%" PRIu32 " packets lost so far: no acknowledgement after %d resends", lost, EspNowLink::maxResends);
		reportedLost = lost;
	}
	if (!running)
		return;
	if (repinNeeded.exchange(false))
	{
		for (const Peer& peer : peers)
			if (peer.pinned)
				applyPeer(peer);
	}
	settle();
	const TxState state = txState;
	if (state == TxState::quiet && esp_timer_get_time() < quietUntil)
		return;
	if (state != TxState::failed && state != TxState::noBuffer && state != TxState::quiet)
		return;
	if (state == TxState::failed && txOnce)
	{
		txState = TxState::idle;
		return;
	}
	/* A resend goes at the rung the failure leaves, and at the lowest one after a pause */
	if (state == TxState::failed)
	{
		learn(false);
		if (txResends < EspNowLink::maxResends && startQuiet())
			return;
	}
	if (txResends == EspNowLink::maxResends)
	{
		lost++;
		txState = TxState::idle;
		return;
	}
	txResends++;
	transmit();
}

bool EspNowPort::canSend()
{
	settle();
	return running && txState == TxState::idle;
}

void EspNowPort::resetRate(const uint8_t* const mac)
{
	Peer* const peer = findPeer(mac);
	if (!peer)
		return;
	const bool lower = peer->ladder.getRung() != 0;
	peer->ladder.reset();
	if (lower && running && esp_now_is_peer_exist(mac))
		applyRate(*peer);
}

uint8_t EspNowPort::getRate(const uint8_t* const mac)
{
	const Peer* const peer = findPeer(mac);
	return peer ? EspNowLink::rates[peer->ladder.getRung()].halfMbps : 0;
}

void EspNowPort::abandon()
{
	settle();
	const TxState state = txState;
	if (state == TxState::failed || state == TxState::noBuffer || state == TxState::quiet)
		txState = TxState::idle;
	else if (state == TxState::inFlight)
		txOnce = true;
}

EspNowPort::SendResult EspNowPort::send(const uint8_t* const mac, const EspNowLink::Header& header, const uint8_t* const frame, const size_t length, const bool once)
{
	if (!canSend())
		return SendResult::busy;
	if (length > EspNowLink::maxFrame || !addPeer(mac))
		return SendResult::failed;

	EspNowLink::Header stamped = header;
	stamped.channel = currentChannel();
	EspNowLink::encodeHeader(txPacket, stamped);
	if (length)
		memcpy(txPacket + EspNowLink::headerSize, frame, length);
	memcpy(txMac, mac, sizeof(txMac));
	txLength = EspNowLink::headerSize + length;
	txResends = 0;
	txOnce = once || header.session == EspNowLink::noSession;
	if (transmit())
		return SendResult::sent;
	/* Not taken: the caller keeps the frame */
	const bool full = txState == TxState::noBuffer;
	txState = TxState::idle;
	return full ? SendResult::busy : SendResult::failed;
}
