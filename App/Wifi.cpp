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
	{
		std::lock_guard<std::mutex> lock(statusLock);
		strlcpy(status.hostname, saved.hostname, sizeof(status.hostname));
		strlcpy(status.savedSsid, saved.ssid, sizeof(status.savedSsid));
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
		post(Command::staConnected);
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
			connectStation(saved);
		if (joinDeadline.due(now) && joining)
			onJoinTimeout();
		if (closeDeadline.due(now) && hasIp)
			leaveSetup();
		if (buttonDeadline.due(now))
			pollButton(now);
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
			if (!setupMode && !staBusy)
				connectStation(saved);
			break;
		case Command::staConnected:
			staLinked = true;
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
	}
}

TickType_t Wifi::ticksUntilDeadline() const
{
	int64_t next = INT64_MAX;
	for (const Deadline* const deadline : {&bootDeadline, &retryDeadline, &joinDeadline, &closeDeadline, &buttonDeadline})
		if (deadline->at)
			next = std::min(next, deadline->at);
	if (next == INT64_MAX)
		return portMAX_DELAY;

	const int64_t remainingMs = (next - esp_timer_get_time() + 999) / 1000;
	return remainingMs > 0 ? pdMS_TO_TICKS(static_cast<uint32_t>(remainingMs)) : 0;
}

void Wifi::boot()
{
	if (!saved.ssid[0])
	{
		ESP_LOGI(tag, "no network saved");
		enterSetup();
		return;
	}

	/* STA_START connects */
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_start());
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
	wifiStarted = true;
	bootDeadline.arm(CONFIG_NETWORK_PROBE_WIFI_STA_TIMEOUT_MS);
}

void Wifi::enterSetup()
{
	setupMode = true;
	{
		std::lock_guard<std::mutex> lock(statusLock);
		status.setupMode = true;
	}
	if (wifiStarted)
		ESP_ERROR_CHECK(esp_wifi_stop());
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
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
	wifiStarted = true;

	ESP_LOGI(tag, "setup mode: access point \"%s\"", deviceId);
	if (saved.ssid[0])
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
	applyHostname(settings.hostname);

	wifi_config_t config = {};
	putField(config.sta.ssid, settings.ssid);
	putField(config.sta.password, settings.password);
	ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_STA, &config));

	ESP_LOGI(tag, "connecting to \"%s\"", settings.ssid);
	const esp_err_t result = esp_wifi_connect();
	if (result == ESP_OK)
	{
		staBusy = true;
		return;
	}

	ESP_LOGE(tag, "esp_wifi_connect() failed: %s", esp_err_to_name(result));
	if (joining)
		finishJoin("the connection could not start");
	else
		retryDeadline.arm(setupMode ? setupRetryMs : reconnectDelayMs);
}

void Wifi::applyHostname(const char* const hostname)
{
	if (!strcmp(activeHostname, hostname))
		return;
	strlcpy(activeHostname, hostname, sizeof(activeHostname));
	/* The DHCP client sends it from the next connection on */
	ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_set_hostname(staNetif, activeHostname));
	ESP_ERROR_CHECK_WITHOUT_ABORT(mdns_hostname_set(activeHostname));
	ESP_LOGI(tag, "hostname %s.local", activeHostname);
}

void Wifi::onDisconnected(const uint32_t reason)
{
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
		ESP_LOGW(tag, "disconnected (reason %lu), reconnecting", static_cast<unsigned long>(reason));
		connectStation(saved);
		return;
	}

	if (saved.ssid[0])
	{
		ESP_LOGI(tag, "\"%s\" not reachable (reason %lu), trying again in %lu s", saved.ssid, static_cast<unsigned long>(reason), static_cast<unsigned long>(setupRetryMs / 1000));
		retryDeadline.arm(setupRetryMs);
	}
}

void Wifi::onGotIp(const uint32_t address)
{
	hasIp = true;
	/* The address of the connection a join is about to drop */
	if (joinAfterDisconnect)
		return;

	bootDeadline.disarm();
	if (joining)
	{
		joining = false;
		joinDeadline.disarm();
		saved = candidate;
		if (saved.save())
			ESP_LOGI(tag, "network \"%s\" saved", saved.ssid);
		else
			ESP_LOGE(tag, "saving network \"%s\" failed", saved.ssid);
	}

	const esp_ip4_addr_t ip = {address};
	char text[sizeof(Status::ip)];
	snprintf(text, sizeof(text), IPSTR, IP2STR(&ip));
	ESP_LOGI(tag, "station IP %s on \"%s\"", text, saved.ssid);
	{
		std::lock_guard<std::mutex> lock(statusLock);
		strlcpy(status.ip, text, sizeof(status.ip));
		strlcpy(status.ssid, saved.ssid, sizeof(status.ssid));
		strlcpy(status.hostname, saved.hostname, sizeof(status.hostname));
		strlcpy(status.savedSsid, saved.ssid, sizeof(status.savedSsid));
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
	/* Once the station is free, setup mode tries the saved network again */
	if (staBusy)
		esp_wifi_disconnect();
	else if (saved.ssid[0])
		retryDeadline.arm(setupRetryMs);
}

void Wifi::scan()
{
	scanPending = false;

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
	if (gpio_get_level(static_cast<gpio_num_t>(CONFIG_NETWORK_PROBE_SETUP_BUTTON_GPIO)) != 0)
		buttonPressedAt = 0;
	else if (!buttonPressedAt)
		buttonPressedAt = now;
	else if (now - buttonPressedAt >= static_cast<int64_t>(buttonHoldMs) * 1000)
	{
		ESP_LOGW(tag, "setup button held: forgetting the saved network and restarting");
		NetworkSettings::erase();
		esp_restart();
	}
	StatusLed::setButtonHold(buttonPressedAt ? buttonHoldMs - static_cast<uint32_t>((now - buttonPressedAt) / 1000) : 0);
	buttonDeadline.arm(buttonPollMs);
#endif
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
