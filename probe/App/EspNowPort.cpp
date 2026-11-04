/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "EspNowPort.hpp"

#include <inttypes.h>
#include <string.h>
#include <unistd.h>

#include <atomic>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace
{
constexpr const char* tag = "espnow_port";
constexpr uint32_t poolSize = 12; /**< Packets the net task has not handled yet; more are dropped. */
constexpr int window = 2;		  /**< Packets in flight: more do not go faster (Tools/espnow_bench). */
constexpr size_t maxPeers = 4;	  /**< Dongles sent to lately; the one unused longest makes room. */
/* 16-QAM: about 2 MB/s with a margin; the rates above come and go with the room (Tools/espnow_bench/RESULTS.md) */
constexpr wifi_phy_mode_t linkMode = WIFI_PHY_MODE_HT20;
constexpr wifi_phy_rate_t linkRate = WIFI_PHY_RATE_MCS3_LGI;

struct Peer
{
	uint8_t mac[6];
	int64_t lastUse;
	bool used;
};

EspNowPort::Packet pool[poolSize];
QueueHandle_t freeQueue = nullptr; /* Packet pointers */
QueueHandle_t rxQueue = nullptr;
int wakeFd = -1;
std::atomic<bool> started{false};
std::atomic<bool> running{false};
std::atomic<int> inFlight{0};
std::atomic<uint32_t> drops{0};
uint32_t reportedDrops = 0;
uint8_t txPacket[EspNowLink::maxPacket];
Peer peers[maxPeers];

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
	packet->length = static_cast<uint16_t>(length);
	memcpy(packet->data, data, length);
	xQueueSend(rxQueue, &packet, 0);
	wake();
}

void onSent(const esp_now_send_info_t* const info, const esp_now_send_status_t status)
{
	inFlight.fetch_sub(1, std::memory_order_relaxed);
	wake();
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

/* A peer for sending: added at the link rate when it is not one (any more), the one unused longest making room */
bool addPeer(const uint8_t* const mac)
{
	const int64_t now = esp_timer_get_time();
	Peer* slot = nullptr;
	for (Peer& peer : peers)
		if (peer.used && !memcmp(peer.mac, mac, sizeof(peer.mac)))
			slot = &peer;
	if (slot && esp_now_is_peer_exist(mac))
	{
		slot->lastUse = now;
		return true;
	}
	if (!slot)
	{
		slot = &peers[0];
		for (Peer& peer : peers)
		{
			if (!peer.used)
			{
				slot = &peer;
				break;
			}
			if (peer.lastUse < slot->lastUse)
				slot = &peer;
		}
		if (slot->used && esp_now_is_peer_exist(slot->mac))
			esp_now_del_peer(slot->mac);
	}

	esp_now_peer_info_t info = {};
	memcpy(info.peer_addr, mac, sizeof(info.peer_addr));
	info.ifidx = WIFI_IF_STA;
	esp_err_t result = esp_now_is_peer_exist(mac) ? ESP_OK : esp_now_add_peer(&info);
	if (result == ESP_OK)
	{
		/* Without it ESP-NOW sends at 1 Mb/s */
		esp_now_rate_config_t rate = {};
		rate.phymode = linkMode;
		rate.rate = linkRate;
		result = esp_now_set_peer_rate_config(mac, &rate);
	}
	if (result != ESP_OK)
	{
		ESP_LOGW(tag, "cannot add peer " MACSTR ": %s", MAC2STR(mac), esp_err_to_name(result));
		slot->used = false;
		return false;
	}
	memcpy(slot->mac, mac, sizeof(slot->mac));
	slot->lastUse = now;
	slot->used = true;
	return true;
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

	uint8_t mac[6];
	esp_read_mac(mac, ESP_MAC_WIFI_STA);
	ESP_LOGI(tag, "ESP-NOW listening as " MACSTR, MAC2STR(mac));
}

void EspNowPort::suspend()
{
	if (!started || !running.exchange(false))
		return;
	esp_now_deinit();
	/* The packets in flight get no callback any more */
	inFlight = 0;
}

void EspNowPort::resume()
{
	if (started && !running)
		running = init();
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
	return xQueueReceive(rxQueue, &packet, 0) == pdTRUE ? packet : nullptr;
}

void EspNowPort::release(Packet* const packet)
{
	xQueueSend(freeQueue, &packet, 0);
}

bool EspNowPort::canSend()
{
	return running && inFlight.load(std::memory_order_relaxed) < window;
}

EspNowPort::SendResult EspNowPort::send(const uint8_t* const mac, const EspNowLink::Kind kind, const uint16_t session, const uint8_t* const frame, const size_t length)
{
	if (!canSend())
		return SendResult::busy;
	if (length > EspNowLink::maxFrame || !addPeer(mac))
		return SendResult::failed;

	EspNowLink::encodeHeader(txPacket, kind, session);
	if (length)
		memcpy(txPacket + EspNowLink::headerSize, frame, length);
	/* Before the send: its callback may come first */
	inFlight.fetch_add(1, std::memory_order_relaxed);
	const esp_err_t result = esp_now_send(mac, txPacket, EspNowLink::headerSize + length);
	if (result == ESP_OK)
		return SendResult::sent;
	inFlight.fetch_sub(1, std::memory_order_relaxed);
	if (result == ESP_ERR_ESPNOW_NO_MEM)
		return SendResult::busy;
	ESP_LOGW(tag, "esp_now_send(): %s", esp_err_to_name(result));
	return SendResult::failed;
}
