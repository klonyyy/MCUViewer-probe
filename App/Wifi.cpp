/*
 * SPDX-FileCopyrightText: 2026 MVProbe contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Wifi.hpp"

#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "sdkconfig.h"

namespace
{
constexpr const char* tag = "wifi";
}  // namespace

void Wifi::start()
{
	events = xEventGroupCreate();

	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	staNetif = esp_netif_create_default_wifi_sta();
	apNetif = esp_netif_create_default_wifi_ap();
	ESP_ERROR_CHECK(esp_netif_set_hostname(staNetif, CONFIG_MVPROBE_HOSTNAME));

	const wifi_init_config_t initConfig = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&initConfig));
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, eventHandler, this));
	ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, eventHandler, this));

	if (strlen(CONFIG_MVPROBE_WIFI_SSID) == 0 || !startStation())
		startAccessPoint();

	startMdns();
}

void Wifi::eventHandler(void* arg, esp_event_base_t base, int32_t id, void* data)
{
	static_cast<Wifi*>(arg)->handleEvent(base, id, data);
}

void Wifi::handleEvent(esp_event_base_t base, int32_t id, void* data)
{
	if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
	{
		esp_wifi_connect();
	}
	else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
	{
		xEventGroupClearBits(events, gotIpBit);
		if (staReconnect)
		{
			const auto* event = static_cast<const wifi_event_sta_disconnected_t*>(data);
			ESP_LOGW(tag, "disconnected (reason %d), reconnecting", event->reason);
			esp_wifi_connect();
		}
	}
	else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
	{
		const auto* event = static_cast<const ip_event_got_ip_t*>(data);
		ESP_LOGI(tag, "station IP " IPSTR, IP2STR(&event->ip_info.ip));
		xEventGroupSetBits(events, gotIpBit);
	}
	else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED)
	{
		ESP_LOGI(tag, "client joined the access point");
	}
}

bool Wifi::startStation()
{
	wifi_config_t config = {};
	strlcpy(reinterpret_cast<char*>(config.sta.ssid), CONFIG_MVPROBE_WIFI_SSID, sizeof(config.sta.ssid));
	strlcpy(reinterpret_cast<char*>(config.sta.password), CONFIG_MVPROBE_WIFI_PASSWORD, sizeof(config.sta.password));

	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
	ESP_ERROR_CHECK(esp_wifi_start());
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

	ESP_LOGI(tag, "connecting to \"%s\"", CONFIG_MVPROBE_WIFI_SSID);
	const EventBits_t bits = xEventGroupWaitBits(events, gotIpBit, pdFALSE, pdTRUE, pdMS_TO_TICKS(CONFIG_MVPROBE_WIFI_STA_TIMEOUT_MS));
	if (bits & gotIpBit)
		return true;

	ESP_LOGW(tag, "no IP address after %d ms", CONFIG_MVPROBE_WIFI_STA_TIMEOUT_MS);
	staReconnect = false;
	ESP_ERROR_CHECK(esp_wifi_stop());
	return false;
}

void Wifi::startAccessPoint()
{
	wifi_config_t config = {};
	strlcpy(reinterpret_cast<char*>(config.ap.ssid), CONFIG_MVPROBE_AP_SSID, sizeof(config.ap.ssid));
	config.ap.ssid_len = strlen(reinterpret_cast<const char*>(config.ap.ssid));
	strlcpy(reinterpret_cast<char*>(config.ap.password), CONFIG_MVPROBE_AP_PASSWORD, sizeof(config.ap.password));
	config.ap.channel = CONFIG_MVPROBE_AP_CHANNEL;
	config.ap.authmode = WIFI_AUTH_WPA2_PSK;
	config.ap.max_connection = apMaxConnections;
	if (strlen(CONFIG_MVPROBE_AP_PASSWORD) < 8)
	{
		ESP_LOGW(tag, "AP password shorter than 8 characters, starting an OPEN access point");
		config.ap.authmode = WIFI_AUTH_OPEN;
		config.ap.password[0] = '\0';
	}

	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
	ESP_ERROR_CHECK(esp_wifi_start());
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

	esp_netif_ip_info_t ipInfo;
	esp_netif_get_ip_info(apNetif, &ipInfo);
	ESP_LOGI(tag, "access point \"%s\" up, IP " IPSTR, CONFIG_MVPROBE_AP_SSID, IP2STR(&ipInfo.ip));
}

void Wifi::startMdns()
{
	ESP_ERROR_CHECK(mdns_init());
	ESP_ERROR_CHECK(mdns_hostname_set(CONFIG_MVPROBE_HOSTNAME));
	ESP_ERROR_CHECK(mdns_instance_name_set("MVProbe"));
	ESP_ERROR_CHECK(mdns_service_add("MVProbe GDB server", "_gdb", "_tcp", CONFIG_MVPROBE_GDB_PORT, nullptr, 0));
	ESP_LOGI(tag, "mDNS hostname %s.local", CONFIG_MVPROBE_HOSTNAME);
}
