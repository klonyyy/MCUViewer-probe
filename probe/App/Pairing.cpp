/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Pairing.hpp"

#include <string.h>
#include <unistd.h>

#include "EspNowPort.hpp"
#include "StatusLed.hpp"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"

namespace
{
constexpr const char* tag = "pairing";
constexpr const char* nvsNamespace = "pairing"; /**< Its own: forgetting the networks empties theirs. */
constexpr const char* nvsKey = "dongles";
constexpr uint8_t formatVersion = 1; /**< A later format converts this one, so pairings survive updates. */
constexpr size_t entrySize = NetworkProbe::macSize + EspNowLink::keySize;
constexpr size_t blobSize = 2 + NetworkProbe::maxPairedDongles * entrySize; /* u8 version, u8 count, entries */

const char* failureName(const NetworkProbe::PairFailure reason)
{
	switch (reason)
	{
		case NetworkProbe::PairFailure::NO_PRESS:
			return "nobody pressed BOOT";
		case NetworkProbe::PairFailure::CONFLICT:
			return "two dongles asked at once";
		case NetworkProbe::PairFailure::PROBE_FULL:
			return "paired with the most dongles already";
		case NetworkProbe::PairFailure::MISMATCH:
			return "the keys did not match";
		case NetworkProbe::PairFailure::STORAGE:
			return "saving failed";
		default:
			return "refused";
	}
}
}  // namespace

std::atomic<bool> Pairing::pressPending{false};
std::atomic<bool> Pairing::erasePending{false};
std::atomic<int> Pairing::wakeFd{-1};

void Pairing::start(const int fd)
{
	wakeFd = fd;
	load();
	if (!count)
	{
		ESP_LOGI(tag, "paired with no dongle: every dongle reaches the probe over ESP-NOW");
		return;
	}
	ESP_LOGI(tag, "paired with %u dongle(s): only they reach the probe over ESP-NOW", static_cast<unsigned>(count));
	for (size_t i = 0; i < count; ++i)
	{
		ESP_LOGI(tag, "  dongle " MACSTR, MAC2STR(dongles[i].mac));
		pinDongle(dongles[i]);
	}
}

void Pairing::load()
{
	count = 0;
	nvs_handle_t handle;
	if (nvs_open(nvsNamespace, NVS_READONLY, &handle) != ESP_OK)
		return;
	uint8_t blob[blobSize];
	size_t length = sizeof(blob);
	const esp_err_t result = nvs_get_blob(handle, nvsKey, blob, &length);
	nvs_close(handle);
	if (result == ESP_ERR_NVS_NOT_FOUND)
		return;
	if (result != ESP_OK || length < 2 || blob[0] != formatVersion || blob[1] > maxDongles || length != 2 + blob[1] * entrySize)
	{
		ESP_LOGE(tag, "the saved pairings cannot be read (%s, format %u, %u bytes): the probe starts unpaired", esp_err_to_name(result), length ? blob[0] : 0, static_cast<unsigned>(length));
		return;
	}
	count = blob[1];
	for (size_t i = 0; i < count; ++i)
	{
		memcpy(dongles[i].mac, blob + 2 + i * entrySize, NetworkProbe::macSize);
		memcpy(dongles[i].key, blob + 2 + i * entrySize + NetworkProbe::macSize, EspNowLink::keySize);
	}
	memset(blob, 0, sizeof(blob));
}

bool Pairing::save() const
{
	uint8_t blob[blobSize];
	blob[0] = formatVersion;
	blob[1] = static_cast<uint8_t>(count);
	for (size_t i = 0; i < count; ++i)
	{
		memcpy(blob + 2 + i * entrySize, dongles[i].mac, NetworkProbe::macSize);
		memcpy(blob + 2 + i * entrySize + NetworkProbe::macSize, dongles[i].key, EspNowLink::keySize);
	}
	nvs_handle_t handle;
	bool written = nvs_open(nvsNamespace, NVS_READWRITE, &handle) == ESP_OK;
	if (written)
	{
		written = nvs_set_blob(handle, nvsKey, blob, 2 + count * entrySize) == ESP_OK && nvs_commit(handle) == ESP_OK;
		nvs_close(handle);
	}
	memset(blob, 0, sizeof(blob));
	return written;
}

int Pairing::find(const uint8_t* const mac) const
{
	for (size_t i = 0; i < count; ++i)
		if (!memcmp(dongles[i].mac, mac, NetworkProbe::macSize))
			return static_cast<int>(i);
	return -1;
}

bool Pairing::add(const uint8_t* const mac, const uint8_t* const key)
{
	int index = find(mac);
	if (index < 0)
	{
		if (count == maxDongles)
			return false;
		index = static_cast<int>(count++);
	}
	memcpy(dongles[index].mac, mac, NetworkProbe::macSize);
	memcpy(dongles[index].key, key, EspNowLink::keySize);
	if (save())
		return true;
	load();
	return false;
}

void Pairing::pinDongle(const Dongle& dongle)
{
	uint8_t lmk[EspNowLink::lmkSize];
	if (!EspNowLink::deriveLmk(dongle.key, lmk) || !EspNowPort::pin(dongle.mac, lmk))
		ESP_LOGE(tag, "dongle " MACSTR ": cannot set up its encrypted peer, it will not reach the probe", MAC2STR(dongle.mac));
	memset(lmk, 0, sizeof(lmk));
}

const uint8_t* Pairing::findKey(const uint8_t* const mac) const
{
	const int index = find(mac);
	return index < 0 ? nullptr : dongles[index].key;
}

NetworkProbe::Pairing Pairing::stateFor(const uint8_t* const mac) const
{
	if (!count)
		return NetworkProbe::Pairing::OPEN;
	return mac && find(mac) >= 0 ? NetworkProbe::Pairing::PAIRED : NetworkProbe::Pairing::LOCKED;
}

bool Pairing::forget(const uint8_t* const mac, const bool keepPeer)
{
	const int index = find(mac);
	if (index < 0)
		return false;
	for (size_t i = static_cast<size_t>(index); i + 1 < count; ++i)
		dongles[i] = dongles[i + 1];
	count--;
	memset(&dongles[count], 0, sizeof(dongles[count]));
	if (!save())
		ESP_LOGE(tag, "saving failed: dongle " MACSTR " is forgotten only until the probe restarts", MAC2STR(mac));
	if (keepPeer)
	{
		memcpy(forgottenMac, mac, sizeof(forgottenMac));
		forgottenPinned = true;
	}
	else
		EspNowPort::unpin(mac);
	if (count)
		ESP_LOGW(tag, "forgot dongle " MACSTR "; %u dongle(s) still paired", MAC2STR(mac), static_cast<unsigned>(count));
	else
		ESP_LOGW(tag, "forgot dongle " MACSTR ": paired with no dongle any more, every dongle reaches the probe", MAC2STR(mac));
	return true;
}

void Pairing::releaseForgotten()
{
	if (!forgottenPinned)
		return;
	forgottenPinned = false;
	if (find(forgottenMac) < 0)
		EspNowPort::unpin(forgottenMac);
}

void Pairing::eraseAll()
{
	const size_t had = count;
	for (size_t i = 0; i < count; ++i)
		EspNowPort::unpin(dongles[i].mac);
	releaseForgotten();
	count = 0;
	memset(dongles, 0, sizeof(dongles));
	finish();

	nvs_handle_t handle;
	bool erased = nvs_open(nvsNamespace, NVS_READWRITE, &handle) == ESP_OK;
	if (erased)
	{
		const esp_err_t result = nvs_erase_key(handle, nvsKey);
		erased = (result == ESP_OK || result == ESP_ERR_NVS_NOT_FOUND) && nvs_commit(handle) == ESP_OK;
		nvs_close(handle);
	}
	if (erased)
		ESP_LOGW(tag, "forgot all %u paired dongle(s): every dongle reaches the probe again", static_cast<unsigned>(had));
	else
		ESP_LOGE(tag, "erasing the pairings in NVS failed: they come back when the probe restarts");
	StatusLed::alert(StatusLed::Alert::failure);
}

bool Pairing::handlePacket(const uint8_t* const mac, const uint8_t* const data, const size_t length, const int64_t now)
{
	if (length < 1)
		return false;
	switch (static_cast<EspNowLink::PairStep>(data[0]))
	{
		case EspNowLink::PairStep::REQUEST:
			if (length == EspNowLink::pairRequestSize)
				onRequest(mac, data + 1, now);
			return false;
		case EspNowLink::PairStep::DONE:
			if (length == EspNowLink::pairDoneSize)
				return onDone(mac, data + 1, now);
			return false;
		default:
			return false;
	}
}

void Pairing::onRequest(const uint8_t* const mac, const uint8_t* const publicKey, const int64_t now)
{
	/* The pairing before is complete: this is a new one */
	if (phase == Phase::paired)
		finish();

	if (phase == Phase::conflict)
	{
		refuse(mac, NetworkProbe::PairFailure::CONFLICT);
		return;
	}
	if (phase == Phase::idle)
	{
		if (count == maxDongles && find(mac) < 0)
		{
			ESP_LOGW(tag, "dongle " MACSTR " asks for pairing: refused, paired with %u dongles already; forget one first", MAC2STR(mac), static_cast<unsigned>(count));
			refuse(mac, NetworkProbe::PairFailure::PROBE_FULL);
			return;
		}
		memcpy(pendingMac, mac, sizeof(pendingMac));
		memcpy(donglePublic, publicKey, sizeof(donglePublic));
		phase = Phase::waiting;
		phaseEnd = now + EspNowLink::pairWindowMs * 1000LL;
		StatusLed::setPairingRequest(true);
		ESP_LOGW(tag, "dongle " MACSTR " asks for pairing%s: press BOOT briefly within %lu s to accept", MAC2STR(mac), find(mac) >= 0 ? " again" : "",
				 static_cast<unsigned long>(EspNowLink::pairWindowMs / 1000));
		sendSignal(mac, EspNowLink::PairStep::WAIT);
		return;
	}

	if (memcmp(mac, pendingMac, sizeof(pendingMac)))
	{
		/* Someone else may be getting in between: nobody gets paired in this window */
		ESP_LOGW(tag, "dongle " MACSTR " asks for pairing while " MACSTR " does: both refused until the window ends", MAC2STR(mac), MAC2STR(pendingMac));
		refuse(mac, NetworkProbe::PairFailure::CONFLICT);
		refuse(pendingMac, NetworkProbe::PairFailure::CONFLICT);
		keyPair.clear();
		memset(pendingKey, 0, sizeof(pendingKey));
		phase = Phase::conflict;
		StatusLed::setPairingRequest(false);
		StatusLed::alert(StatusLed::Alert::failure);
		return;
	}

	if (phase == Phase::accepted && !memcmp(publicKey, donglePublic, sizeof(donglePublic)))
	{
		/* The ACCEPT got lost */
		sendAccept();
		return;
	}
	if (phase == Phase::accepted)
	{
		/* The dongle started over, with a new key pair: that takes a new press */
		ESP_LOGW(tag, "dongle " MACSTR " started the pairing over: press BOOT briefly again to accept", MAC2STR(mac));
		phaseEnd = now + EspNowLink::pairWindowMs * 1000LL;
		phase = Phase::waiting;
		StatusLed::setPairingRequest(true);
	}
	memcpy(donglePublic, publicKey, sizeof(donglePublic));
	sendSignal(mac, EspNowLink::PairStep::WAIT);
}

const uint8_t* Pairing::getWaitingDongle() const
{
	return phase == Phase::waiting ? pendingMac : nullptr;
}

void Pairing::confirm(const int64_t now)
{
	if (phase != Phase::waiting)
	{
		ESP_LOGI(tag, "BOOT pressed briefly: no pairing request to accept");
		return;
	}
	StatusLed::setPairingRequest(false);
	if (!keyPair.generate() || !keyPair.agree(donglePublic, false, pendingMac, EspNowPort::getOwnMac(), pendingKey) || !EspNowLink::deriveConfirm(pendingKey, EspNowLink::Direction::toDongle, probeConfirm))
	{
		ESP_LOGE(tag, "the key exchange with dongle " MACSTR " failed", MAC2STR(pendingMac));
		refuse(pendingMac, NetworkProbe::PairFailure::MISMATCH);
		StatusLed::alert(StatusLed::Alert::failure);
		finish();
		return;
	}
	ESP_LOGI(tag, "BOOT pressed: accepting dongle " MACSTR ", waiting for it to confirm the key", MAC2STR(pendingMac));
	phase = Phase::accepted;
	phaseEnd = now + EspNowLink::pairDoneTimeoutMs * 1000LL;
	sendAccept();
}

bool Pairing::onDone(const uint8_t* const mac, const uint8_t* const confirmField, const int64_t now)
{
	const bool fromPending = !memcmp(mac, pendingMac, sizeof(pendingMac));
	if (!fromPending || (phase != Phase::accepted && phase != Phase::paired))
		return false;

	uint8_t expected[EspNowLink::confirmSize];
	const bool matches = EspNowLink::deriveConfirm(pendingKey, EspNowLink::Direction::toProbe, expected) && EspNowLink::sameBytes(confirmField, expected, sizeof(expected));
	if (phase == Phase::paired)
	{
		/* The PAIRED got lost */
		if (matches)
			sendSignal(mac, EspNowLink::PairStep::PAIRED);
		return false;
	}
	if (!matches)
	{
		ESP_LOGW(tag, "dongle " MACSTR ": the keys do not match, pairing refused", MAC2STR(mac));
		refuse(mac, NetworkProbe::PairFailure::MISMATCH);
		StatusLed::alert(StatusLed::Alert::failure);
		finish();
		return false;
	}

	const bool again = find(mac) >= 0;
	if (!add(mac, pendingKey))
	{
		const bool full = !again && count == maxDongles;
		ESP_LOGE(tag, "dongle " MACSTR ": %s, pairing refused", MAC2STR(mac), full ? "paired with the most dongles already" : "saving the pairing failed");
		refuse(mac, full ? NetworkProbe::PairFailure::PROBE_FULL : NetworkProbe::PairFailure::STORAGE);
		StatusLed::alert(StatusLed::Alert::failure);
		finish();
		return false;
	}
	pinDongle(dongles[find(mac)]);
	ESP_LOGW(tag, "paired with dongle " MACSTR "%s (%u of %u): only paired dongles reach the probe over ESP-NOW", MAC2STR(mac), again ? " again, with a new key" : "", static_cast<unsigned>(count),
			 static_cast<unsigned>(maxDongles));
	StatusLed::paired();
	sendSignal(mac, EspNowLink::PairStep::PAIRED);
	keyPair.clear();
	phase = Phase::paired;
	phaseEnd = now + EspNowLink::pairDoneTimeoutMs * 1000LL;
	return true;
}

void Pairing::service(const int64_t now)
{
	if (phase == Phase::idle || now < phaseEnd)
		return;
	switch (phase)
	{
		case Phase::waiting:
			ESP_LOGW(tag, "nobody pressed BOOT within %lu s: pairing of dongle " MACSTR " refused", static_cast<unsigned long>(EspNowLink::pairWindowMs / 1000), MAC2STR(pendingMac));
			refuse(pendingMac, NetworkProbe::PairFailure::NO_PRESS);
			StatusLed::alert(StatusLed::Alert::failure);
			break;
		case Phase::accepted:
			ESP_LOGW(tag, "dongle " MACSTR " did not confirm the key: pairing dropped", MAC2STR(pendingMac));
			StatusLed::alert(StatusLed::Alert::failure);
			break;
		case Phase::conflict:
			ESP_LOGI(tag, "the pairing window is over: requests are taken again");
			break;
		default:
			break;
	}
	finish();
}

void Pairing::finish()
{
	phase = Phase::idle;
	keyPair.clear();
	memset(pendingKey, 0, sizeof(pendingKey));
	memset(probeConfirm, 0, sizeof(probeConfirm));
	StatusLed::setPairingRequest(false);
}

void Pairing::send(const uint8_t* const mac, const uint8_t* const data, const size_t length)
{
	/* Broadcast, addressed to the dongle: it may not know the probe's key, so nothing is encrypted */
	EspNowLink::Header header = {EspNowLink::Kind::PAIR, EspNowLink::noSession, 0, 0, 0, {}};
	EspNowLink::addressTo(header, mac);
	if (EspNowPort::send(EspNowLink::broadcastMac, header, data, length) != EspNowPort::SendResult::sent)
		ESP_LOGD(tag, "pairing step %u not sent: the radio is busy, the dongle asks again", data[0]);
}

void Pairing::sendSignal(const uint8_t* const mac, const EspNowLink::PairStep step)
{
	const uint8_t data[EspNowLink::pairSignalSize] = {NetworkProbe::toByte(step)};
	send(mac, data, sizeof(data));
}

void Pairing::sendAccept()
{
	uint8_t data[EspNowLink::pairAcceptSize];
	data[0] = NetworkProbe::toByte(EspNowLink::PairStep::ACCEPT);
	memcpy(data + 1, keyPair.getPublic(), EspNowLink::publicKeySize);
	memcpy(data + 1 + EspNowLink::publicKeySize, probeConfirm, EspNowLink::confirmSize);
	send(pendingMac, data, sizeof(data));
}

void Pairing::refuse(const uint8_t* const mac, const NetworkProbe::PairFailure reason)
{
	ESP_LOGD(tag, "refusing dongle " MACSTR ": %s", MAC2STR(mac), failureName(reason));
	const uint8_t data[EspNowLink::pairRefuseSize] = {NetworkProbe::toByte(EspNowLink::PairStep::REFUSE), NetworkProbe::toByte(reason)};
	send(mac, data, sizeof(data));
}

void Pairing::notifyPress()
{
	pressPending = true;
	wakeNet();
}

void Pairing::notifyErase()
{
	erasePending = true;
	wakeNet();
}

bool Pairing::takePress()
{
	return pressPending.exchange(false);
}

bool Pairing::takeErase()
{
	return erasePending.exchange(false);
}

void Pairing::wakeNet()
{
	const int fd = wakeFd;
	if (fd < 0)
		return;
	const uint64_t one = 1;
	write(fd, &one, sizeof(one));
}
