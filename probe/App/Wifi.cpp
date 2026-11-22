/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Wifi.hpp"

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <initializer_list>
#include <memory>
#include <new>

#include "EspNowPort.hpp"
#include "EspNowRate.hpp"
#include "Pairing.hpp"
#include "StatusLed.hpp"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "sdkconfig.h"

namespace
{
constexpr const char* tag = "wifi";

/* Copies text into a field of a Wi-Fi configuration that was zeroed; a full field has no terminator */
template <size_t size>
void putField(uint8_t (&field)[size], const char* const text)
{
	memcpy(field, text, std::min(strlen(text), size));
}

bool isAuthFailure(const uint32_t reason)
{
	return reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || reason == WIFI_REASON_HANDSHAKE_TIMEOUT;
}

const char* describeFailure(const uint32_t reason)
{
	switch (reason)
	{
		case WIFI_REASON_NO_AP_FOUND:
		case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
			return "the network was not found (only 2.4 GHz networks work)";
		case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
		case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
			return "the network's security mode does not match the password";
		case WIFI_REASON_AUTH_FAIL:
		case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
		case WIFI_REASON_HANDSHAKE_TIMEOUT:
			return "the password is probably wrong";
		default:
			return "the connection failed";
	}
}
}  // namespace

void Wifi::start()
{
	events = xEventGroupCreate();
	queue = xQueueCreate(queueLength, sizeof(Message));
	NetworkSettings::getDeviceId(deviceId, sizeof(deviceId));
	saved.load();
	radio.load();
	fieldMode = radio.fieldMode;
	fieldChannel = radio.fieldChannel;
	espNowEnabled = radio.espNow;
	if (!radio.espNow)
		ESP_LOGI(tag, "ESP-NOW is off: dongles do not reach the probe");
	{
		std::lock_guard<std::mutex> lock(statusLock);
		strlcpy(status.hostname, saved.hostname, sizeof(status.hostname));
		strlcpy(status.savedSsid, saved.count ? saved.networks[0].ssid : "", sizeof(status.savedSsid));
		status.savedCount = static_cast<uint8_t>(saved.count);
	}
	ESP_LOGI(tag, "device id %s", deviceId);

	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	staNetif = esp_netif_create_default_wifi_sta();
	apNetif = esp_netif_create_default_wifi_ap();

	const wifi_init_config_t initConfig = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&initConfig));
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, eventHandler, this));
	ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, eventHandler, this));
	startMdns();
	startButton();

	xTaskCreatePinnedToCore(taskEntry, "wifi", taskStackSize, this, taskPriority, nullptr, taskCore);
	xEventGroupWaitBits(events, readyBit, pdFALSE, pdTRUE, portMAX_DELAY);
}

void Wifi::join(const NetworkSettings& settings)
{
	{
		std::lock_guard<std::mutex> lock(statusLock);
		requested = settings;
		/* The page asks for the status right after: show the join at once */
		status.state = JoinState::connecting;
		strlcpy(status.ssid, settings.ssid, sizeof(status.ssid));
		strlcpy(status.hostname, settings.hostname, sizeof(status.hostname));
		status.error[0] = '\0';
	}
	post(Command::join);
}

Wifi::Status Wifi::getStatus()
{
	std::lock_guard<std::mutex> lock(statusLock);
	return status;
}

void Wifi::getHostname(char* const hostname, const size_t size)
{
	std::lock_guard<std::mutex> lock(statusLock);
	strlcpy(hostname, activeHostname, size);
}

void Wifi::requestScan(const bool force)
{
	{
		std::lock_guard<std::mutex> lock(statusLock);
		if (scanRequested || (scanned && !force))
			return;
		scanRequested = true;
	}
	post(Command::scan);
}

RadioSettings Wifi::getRadio()
{
	std::lock_guard<std::mutex> lock(statusLock);
	return radio;
}

const char* Wifi::setRadio(const RadioSettings& settings, bool& restarting)
{
	restarting = false;
	const char* const error = settings.validate();
	if (error)
		return error;
	if (!settings.save())
		return "The settings could not be saved.";
	{
		std::lock_guard<std::mutex> lock(statusLock);
		radio = settings;
	}
	espNowEnabled = settings.espNow;
	ESP_LOGI(tag, "ESP-NOW %s, field mode %s, field channel %u", settings.espNow ? "on" : "off", settings.fieldMode ? "on" : "off", settings.fieldChannel);

	/* The page is there only out of field mode: field mode on means a restart into it */
	if (settings.fieldMode && !fieldMode)
	{
		restarting = true;
		post(Command::restart);
	}
	return nullptr;
}

void Wifi::holdRadio(const uint8_t channel)
{
	/* The Wi-Fi task waits in it, and would see the hold only after it */
	if (channel && ownScan)
		esp_wifi_scan_stop();
	post(Command::radioHold, channel);
}

size_t Wifi::getNetworks(Network* const out, const size_t maxCount, bool& scanning)
{
	std::lock_guard<std::mutex> lock(statusLock);
	scanning = scanRequested;
	const size_t count = std::min(networkCount, maxCount);
	std::copy(networks, networks + count, out);
	return count;
}

void Wifi::eventHandler(void* arg, esp_event_base_t base, int32_t id, void* data)
{
	static_cast<Wifi*>(arg)->handleEvent(base, id, data);
}

void Wifi::handleEvent(esp_event_base_t base, int32_t id, void* data)
{
	if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
		post(Command::staStart);
	else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED)
		post(Command::staConnected, static_cast<const wifi_event_sta_connected_t*>(data)->channel);
	else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
		post(Command::staDisconnected, static_cast<const wifi_event_sta_disconnected_t*>(data)->reason);
	else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
		post(Command::gotIp, static_cast<const ip_event_got_ip_t*>(data)->ip_info.ip.addr);
	else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START)
		post(Command::apStart);
	else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED)
		ESP_LOGI(tag, "client joined the access point");
}

void Wifi::post(const Command command, const uint32_t value)
{
	const Message message = {command, value};
	if (xQueueSend(queue, &message, pdMS_TO_TICKS(postTimeoutMs)) != pdTRUE)
		ESP_LOGE(tag, "command queue full, command %u dropped", static_cast<unsigned>(command));
}

void Wifi::taskEntry(void* const arg)
{
	static_cast<Wifi*>(arg)->run();
}

void Wifi::run()
{
	boot();
	while (true)
	{
		/* A join from the setup page shows as connecting, though the access point stays up */
		if (fieldMode)
			StatusLed::setNetwork(StatusLed::Network::field);
		else
			StatusLed::setNetwork(hasIp ? StatusLed::Network::online : (setupMode && !joining ? StatusLed::Network::setup : StatusLed::Network::connecting));

		Message message;
		if (xQueueReceive(queue, &message, ticksUntilDeadline()) == pdTRUE)
			handle(message);

		const int64_t now = esp_timer_get_time();
		if (bootDeadline.due(now) && !hasIp)
		{
			ESP_LOGW(tag, "no IP address after %d ms", CONFIG_NETWORK_PROBE_WIFI_STA_TIMEOUT_MS);
			enterSetup();
		}
		if (retryDeadline.due(now) && !staBusy && !joining)
			retrySaved();
		if (joinDeadline.due(now) && joining)
			onJoinTimeout();
		if (closeDeadline.due(now) && hasIp)
			leaveSetup();
		if (buttonDeadline.due(now))
			pollButton(now);
		if (restartDeadline.due(now))
		{
			ESP_LOGW(tag, "restarting into field mode");
			esp_restart();
		}
		/* A scan cannot run while the station connects */
		if (scanPending && (!staBusy || hasIp))
			scan();
	}
}

void Wifi::handle(const Message& message)
{
	switch (message.command)
	{
		case Command::staStart:
			if (fieldMode)
				parkRadio();
			else if (!setupMode && !staBusy)
				connectSaved();
			break;
		case Command::staConnected:
			staLinked = true;
			homeChannel = static_cast<uint8_t>(message.value);
			roaming = false;
			break;
		case Command::staDisconnected:
			onDisconnected(message.value);
			break;
		case Command::gotIp:
			onGotIp(message.value);
			break;
		case Command::apStart:
			xEventGroupSetBits(events, readyBit);
			break;
		case Command::join:
			onJoin();
			break;
		case Command::scan:
			scanPending = true;
			break;
		case Command::radioHold:
			onRadioHold(static_cast<uint8_t>(message.value));
			break;
		case Command::restart:
			restartDeadline.arm(restartDelayMs);
			break;
	}
}

TickType_t Wifi::ticksUntilDeadline() const
{
	int64_t next = INT64_MAX;
	for (const Deadline* const deadline : {&bootDeadline, &retryDeadline, &joinDeadline, &closeDeadline, &buttonDeadline, &restartDeadline})
		if (deadline->at)
			next = std::min(next, deadline->at);
	if (next == INT64_MAX)
		return portMAX_DELAY;

	const int64_t remainingMs = (next - esp_timer_get_time() + 999) / 1000;
	return remainingMs > 0 ? pdMS_TO_TICKS(static_cast<uint32_t>(remainingMs)) : 0;
}

void Wifi::boot()
{
	if (fieldMode)
	{
		startField();
		return;
	}
	if (!saved.count)
	{
		ESP_LOGI(tag, "no network saved");
		enterSetup();
		return;
	}

	/* STA_START connects; ESP-NOW's long range rate needs LR next to b/g/n, which joins ordinary networks still */
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_start());
	ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_protocol(WIFI_IF_STA, EspNowLink::protocols));
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
	wifiStarted = true;
	bootDeadline.arm(CONFIG_NETWORK_PROBE_WIFI_STA_TIMEOUT_MS);
}

void Wifi::startField()
{
	/* As out of field mode, but STA_START only parks the radio */
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_start());
	ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_protocol(WIFI_IF_STA, EspNowLink::protocols));
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
	wifiStarted = true;
	parkRadio();
	ESP_LOGI(tag, "field mode: no network, ESP-NOW alone on channel %u; hold the setup button for %lu s and let go to leave it", fieldChannel, static_cast<unsigned long>(buttonHoldMs / 1000));
	xEventGroupSetBits(events, readyBit);
}

void Wifi::enterSetup()
{
	setupMode = true;
	{
		std::lock_guard<std::mutex> lock(statusLock);
		status.setupMode = true;
	}
	if (wifiStarted)
	{
		EspNowPort::suspend();
		ESP_ERROR_CHECK(esp_wifi_stop());
	}
	staBusy = false;
	staLinked = false;

	wifi_config_t config = {};
	putField(config.ap.ssid, deviceId);
	config.ap.ssid_len = strlen(deviceId);
	putField(config.ap.password, CONFIG_NETWORK_PROBE_AP_PASSWORD);
	config.ap.channel = CONFIG_NETWORK_PROBE_AP_CHANNEL;
	config.ap.authmode = WIFI_AUTH_WPA2_PSK;
	config.ap.max_connection = apMaxConnections;
	if (strlen(CONFIG_NETWORK_PROBE_AP_PASSWORD) < 8)
	{
		ESP_LOGW(tag, "AP password shorter than 8 characters, starting an OPEN access point");
		config.ap.authmode = WIFI_AUTH_OPEN;
		memset(config.ap.password, 0, sizeof(config.ap.password));
	}

	/* Before the access point starts: its DHCP server takes the captive portal option only while stopped */
	portal.start(apNetif);

	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
	ESP_ERROR_CHECK(esp_wifi_start());
	ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_protocol(WIFI_IF_STA, EspNowLink::protocols));
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
	wifiStarted = true;
	EspNowPort::resume();

	ESP_LOGI(tag, "setup mode: access point \"%s\"", deviceId);
	if (saved.count)
		retryDeadline.arm(setupRetryMs);
}

void Wifi::leaveSetup()
{
	portal.stop();
	ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_STA));
	setupMode = false;
	{
		std::lock_guard<std::mutex> lock(statusLock);
		status.setupMode = false;
	}
	ESP_LOGI(tag, "setup done, access point closed");
}

void Wifi::connectStation(const NetworkSettings& settings)
{
	active = settings;
	applyHostname(settings.hostname);

	wifi_config_t config = {};
	putField(config.sta.ssid, settings.ssid);
	putField(config.sta.password, settings.password);
	ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_STA, &config));

	ESP_LOGI(tag, "connecting to \"%s\"", settings.ssid);
	/* The connection looks for the network on every channel */
	roaming = true;
	const esp_err_t result = esp_wifi_connect();
	if (result == ESP_OK)
	{
		staBusy = true;
		return;
	}
	roaming = false;

	ESP_LOGE(tag, "esp_wifi_connect() failed: %s", esp_err_to_name(result));
	if (joining)
		finishJoin("the connection could not start");
	else
		retryDeadline.arm(setupMode ? setupRetryMs : reconnectDelayMs);
}

void Wifi::connectSaved()
{
	orderCount = 0;
	orderNext = 0;
	if (saved.count > 1)
	{
		ownScan = true;
		scan();
		ownScan = false;
		/* The scan lists the networks strongest first, and so does the order */
		std::lock_guard<std::mutex> lock(statusLock);
		for (size_t n = 0; n < networkCount; ++n)
			for (size_t i = 0; i < saved.count; ++i)
				if (!strcmp(networks[n].ssid, saved.networks[i].ssid))
					order[orderCount++] = static_cast<uint8_t>(i);
	}
	/* A single network, or none in sight: hidden networks do not show in a scan, so try them all */
	if (!orderCount)
		for (size_t i = 0; i < saved.count; ++i)
			order[orderCount++] = static_cast<uint8_t>(i);
	connectNextSaved();
}

bool Wifi::connectNextSaved()
{
	if (orderNext >= orderCount)
		return false;
	connectStation(saved.networks[order[orderNext++]]);
	return true;
}

void Wifi::applyHostname(const char* const hostname)
{
	if (!strcmp(activeHostname, hostname))
		return;
	{
		std::lock_guard<std::mutex> lock(statusLock);
		strlcpy(activeHostname, hostname, sizeof(activeHostname));
	}
	/* The DHCP client sends it from the next connection on */
	ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_set_hostname(staNetif, activeHostname));
	ESP_ERROR_CHECK_WITHOUT_ABORT(mdns_hostname_set(activeHostname));
	ESP_LOGI(tag, "hostname %s.local", activeHostname);
}

void Wifi::onDisconnected(const uint32_t reason)
{
	const bool wasLinked = staLinked;
	staBusy = false;
	staLinked = false;
	if (hasIp)
	{
		hasIp = false;
		std::lock_guard<std::mutex> lock(statusLock);
		status.ip[0] = '\0';
		if (status.state == JoinState::connected)
			status.state = JoinState::idle;
	}

	if (joinAfterDisconnect)
	{
		joinAfterDisconnect = false;
		connectStation(candidate);
		return;
	}

	if (joining)
	{
		if (!isAuthFailure(reason) && ++joinAttempts < maxJoinAttempts)
		{
			ESP_LOGW(tag, "joining \"%s\" failed (reason %lu), trying again", candidate.ssid, static_cast<unsigned long>(reason));
			connectStation(candidate);
			return;
		}
		char error[sizeof(Status::error)];
		snprintf(error, sizeof(error), "%s (reason %lu)", describeFailure(reason), static_cast<unsigned long>(reason));
		finishJoin(error);
		return;
	}

	if (!setupMode)
	{
		ESP_LOGW(tag, "disconnected (reason %lu)", static_cast<unsigned long>(reason));
		/* A lost link starts over with the strongest network at once, a failed attempt goes on with the next one */
		if (wasLinked)
		{
			roundPauseMs = roundPauseMinMs;
			retrySaved();
		}
		else if (radioHeld)
			deferToRelease();
		else if (!connectNextSaved())
		{
			ESP_LOGI(tag, "no saved network reachable, trying again in %lu s", static_cast<unsigned long>(roundPauseMs / 1000));
			parkRadio();
			retryDeadline.arm(roundPauseMs);
			roundPauseMs = std::min(roundPauseMs * 2, roundPauseMaxMs);
		}
		return;
	}

	if (saved.count)
	{
		if (wasLinked)
			orderNext = orderCount;
		if (radioHeld)
		{
			deferToRelease();
			return;
		}
		if (connectNextSaved())
			return;
		ESP_LOGI(tag, "no saved network reachable (reason %lu), trying again in %lu s", static_cast<unsigned long>(reason), static_cast<unsigned long>(setupRetryMs / 1000));
		parkRadio();
		retryDeadline.arm(setupRetryMs);
	}
}

void Wifi::retrySaved()
{
	if (radioHeld)
		deferToRelease();
	else
		connectSaved();
}

void Wifi::deferToRelease()
{
	ESP_LOGI(tag, "an ESP-NOW session holds the radio: the saved networks wait until it ends");
	retryOnRelease = true;
	parkRadio();
}

void Wifi::parkRadio()
{
	roaming = false;
	/* The access point keeps the radio on its own channel */
	if (setupMode)
		return;
	uint8_t channel = homeChannel ? homeChannel : CONFIG_NETWORK_PROBE_AP_CHANNEL;
	if (fieldMode)
		channel = fieldChannel;
	else if (radioHeld)
		channel = holdChannel;
	const esp_err_t result = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
	if (result != ESP_OK)
		ESP_LOGW(tag, "cannot put the radio on channel %u: %s", channel, esp_err_to_name(result));
}

void Wifi::onRadioHold(const uint8_t channel)
{
	if (channel)
	{
		radioHeld = true;
		holdChannel = channel;
		/* An attempt of the station's own takes the radio off the channel: it stops, and its round waits */
		if (staBusy && !staLinked && !joining)
			ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_disconnect());
		/* Idle between rounds: on to the session's channel, should the dongle have found the probe elsewhere */
		else if (!staBusy)
			parkRadio();
		return;
	}

	radioHeld = false;
	if (retryOnRelease)
	{
		retryOnRelease = false;
		retryDeadline.arm(releaseDelayMs);
	}
}

void Wifi::onGotIp(const uint32_t address)
{
	hasIp = true;
	/* The address of the connection a join is about to drop */
	if (joinAfterDisconnect)
		return;

	bootDeadline.disarm();
	roundPauseMs = roundPauseMinMs;
	if (joining)
	{
		joining = false;
		joinDeadline.disarm();
		saved.add(candidate);
		/* The indexes of the round are stale now */
		orderCount = 0;
		orderNext = 0;
		if (saved.save())
			ESP_LOGI(tag, "network \"%s\" saved", candidate.ssid);
		else
			ESP_LOGE(tag, "saving network \"%s\" failed", candidate.ssid);
	}

	const esp_ip4_addr_t ip = {address};
	char text[sizeof(Status::ip)];
	snprintf(text, sizeof(text), IPSTR, IP2STR(&ip));
	ESP_LOGI(tag, "station IP %s on \"%s\"", text, active.ssid);
	{
		std::lock_guard<std::mutex> lock(statusLock);
		strlcpy(status.ip, text, sizeof(status.ip));
		strlcpy(status.ssid, active.ssid, sizeof(status.ssid));
		strlcpy(status.hostname, saved.hostname, sizeof(status.hostname));
		strlcpy(status.savedSsid, saved.networks[0].ssid, sizeof(status.savedSsid));
		status.savedCount = static_cast<uint8_t>(saved.count);
		if (setupMode)
		{
			status.state = JoinState::connected;
			status.error[0] = '\0';
		}
	}

	if (setupMode)
		closeDeadline.arm(setupCloseMs);
	xEventGroupSetBits(events, readyBit);
}

void Wifi::onJoin()
{
	{
		std::lock_guard<std::mutex> lock(statusLock);
		candidate = requested;
	}
	if (!setupMode)
		return;

	ESP_LOGI(tag, "joining \"%s\" as %s.local", candidate.ssid, candidate.hostname);
	joining = true;
	joinAttempts = 0;
	orderCount = 0;
	orderNext = 0;
	retryDeadline.disarm();
	closeDeadline.disarm();
	joinDeadline.arm(joinTimeoutMs);
	if (staBusy)
	{
		joinAfterDisconnect = true;
		esp_wifi_disconnect();
	}
	else
		connectStation(candidate);
}

void Wifi::onJoinTimeout()
{
	if (joinAfterDisconnect)
	{
		/* The disconnection never came; the next one must not start the candidate again */
		joinAfterDisconnect = false;
		finishJoin("the probe could not leave its previous connection");
		return;
	}
	finishJoin(staLinked ? "the network gave no IP address" : "the connection timed out");
}

void Wifi::finishJoin(const char* const error)
{
	ESP_LOGW(tag, "joining \"%s\" failed: %s", candidate.ssid, error);
	StatusLed::alert(StatusLed::Alert::failure);
	joining = false;
	joinDeadline.disarm();
	{
		std::lock_guard<std::mutex> lock(statusLock);
		status.state = JoinState::failed;
		strlcpy(status.error, error, sizeof(status.error));
		strlcpy(status.hostname, saved.hostname, sizeof(status.hostname));
	}

	applyHostname(saved.hostname);
	parkRadio();
	/* Once the station is free, setup mode tries the saved networks again */
	if (staBusy)
		esp_wifi_disconnect();
	else if (saved.count)
		retryDeadline.arm(setupRetryMs);
}

void Wifi::scan()
{
	scanPending = false;
	roaming = true;

	std::unique_ptr<wifi_ap_record_t[]> records(new (std::nothrow) wifi_ap_record_t[maxScanRecords]);
	uint16_t count = maxScanRecords;
	esp_err_t result = records ? esp_wifi_scan_start(nullptr, true) : ESP_ERR_NO_MEM;
	if (result == ESP_OK)
		result = esp_wifi_scan_get_ap_records(&count, records.get());
	if (result != ESP_OK)
	{
		ESP_LOGW(tag, "scan failed: %s", esp_err_to_name(result));
		esp_wifi_clear_ap_list();
		count = 0;
	}
	/* Back on the channel of the network, or of the access point; else a connection or parkRadio() follows */
	if (staLinked || setupMode)
		roaming = false;

	std::lock_guard<std::mutex> lock(statusLock);
	networkCount = 0;
	/* The records come strongest first: keep the first of every SSID, hidden networks have none */
	for (uint16_t i = 0; i < count && networkCount < maxNetworks; ++i)
	{
		const wifi_ap_record_t& record = records[i];
		const char* const ssid = reinterpret_cast<const char*>(record.ssid);
		if (!ssid[0] || std::any_of(networks, networks + networkCount, [ssid](const Network& network)
									{ return !strcmp(network.ssid, ssid); }))
			continue;
		Network& network = networks[networkCount++];
		strlcpy(network.ssid, ssid, sizeof(network.ssid));
		network.rssi = record.rssi;
		network.secured = record.authmode != WIFI_AUTH_OPEN && record.authmode != WIFI_AUTH_OWE;
	}
	scanned = true;
	scanRequested = false;
}

void Wifi::startMdns()
{
	ESP_ERROR_CHECK(mdns_init());
	applyHostname(saved.hostname);
	/* Without instance names of their own, the services are named after the hostname, which tells the probes apart */
	ESP_ERROR_CHECK(mdns_service_add(nullptr, "_gdb", "_tcp", CONFIG_NETWORK_PROBE_GDB_PORT, nullptr, 0));
	ESP_ERROR_CHECK(mdns_service_add(nullptr, "_network-probe", "_tcp", CONFIG_NETWORK_PROBE_PORT, nullptr, 0));
}

void Wifi::startButton()
{
#if CONFIG_NETWORK_PROBE_SETUP_BUTTON_GPIO >= 0
	const gpio_config_t config = {
		.pin_bit_mask = 1ULL << CONFIG_NETWORK_PROBE_SETUP_BUTTON_GPIO,
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_ENABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	ESP_ERROR_CHECK(gpio_config(&config));
	buttonDeadline.arm(buttonPollMs);
#endif
}

void Wifi::pollButton(const int64_t now)
{
#if CONFIG_NETWORK_PROBE_SETUP_BUTTON_GPIO >= 0
	const bool down = gpio_get_level(static_cast<gpio_num_t>(CONFIG_NETWORK_PROBE_SETUP_BUTTON_GPIO)) == 0;
	if (down && !buttonPressedAt && now - buttonReleasedAt >= static_cast<int64_t>(buttonLockoutMs) * 1000)
		buttonPressedAt = now;
	if (buttonPressedAt)
	{
		const int64_t heldMs = (now - buttonPressedAt) / 1000;
		if (!down)
		{
			buttonPressedAt = 0;
			buttonReleasedAt = now;
			StatusLed::setButtonHold(StatusLed::Hold::none);
			onButtonReleased(heldMs, now);
		}
		else if (heldMs >= eraseHoldMs)
			StatusLed::setButtonHold(StatusLed::Hold::erase);
		else if (heldMs >= buttonHoldMs)
			StatusLed::setButtonHold(StatusLed::Hold::networks);
		else
			StatusLed::setButtonHold(StatusLed::Hold::counting, buttonHoldMs - static_cast<uint32_t>(heldMs));
	}
	if (eraseArmedUntil && now >= eraseArmedUntil)
	{
		eraseArmedUntil = 0;
		StatusLed::setEraseArmed(false);
		ESP_LOGI(tag, "no short press within %lu s: the pairings stay", static_cast<unsigned long>(eraseConfirmMs / 1000));
	}
	buttonDeadline.arm(buttonPollMs);
#endif
}

void Wifi::onButtonReleased(const int64_t heldMs, const int64_t now)
{
	if (heldMs < shortPressMaxMs)
	{
		if (!eraseArmedUntil)
		{
			/* The net task tells whether a pairing request waited for it */
			Pairing::notifyPress();
			return;
		}
		eraseArmedUntil = 0;
		StatusLed::setEraseArmed(false);
		ESP_LOGW(tag, "setup button: erase of every pairing confirmed");
		Pairing::notifyErase();
		return;
	}
	if (heldMs >= eraseHoldMs)
	{
		eraseArmedUntil = now + static_cast<int64_t>(eraseConfirmMs) * 1000;
		StatusLed::setEraseArmed(true);
		ESP_LOGW(tag, "setup button held for %lld s: press it briefly within %lu s to forget every paired dongle", static_cast<long long>(heldMs / 1000), static_cast<unsigned long>(eraseConfirmMs / 1000));
		return;
	}
	if (heldMs >= buttonHoldMs)
	{
		leaveFieldOrForget();
		return;
	}
	ESP_LOGI(tag, "setup button released after %lld ms: nothing to do (3 s: %s, 10 s: forget the paired dongles)", static_cast<long long>(heldMs), fieldMode ? "leave field mode" : "forget the networks");
}

void Wifi::leaveFieldOrForget()
{
	if (fieldMode)
	{
		ESP_LOGW(tag, "setup button held: leaving field mode and restarting; the saved networks stay");
		RadioSettings settings = getRadio();
		settings.fieldMode = false;
		if (!settings.save())
			ESP_LOGE(tag, "saving the radio settings failed");
	}
	else
	{
		ESP_LOGW(tag, "setup button held: forgetting the saved networks and restarting; the paired dongles stay");
		SavedNetworks::erase();
	}
	esp_restart();
}

void Wifi::Deadline::arm(const uint32_t ms)
{
	at = esp_timer_get_time() + static_cast<int64_t>(ms) * 1000;
}

bool Wifi::Deadline::due(const int64_t now)
{
	if (!at || now < at)
		return false;
	at = 0;
	return true;
}
