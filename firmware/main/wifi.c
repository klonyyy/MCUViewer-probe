/*
 * SPDX-FileCopyrightText: 2026 MVProbe contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Wi-Fi station with access point fallback, and mDNS announcement.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "sdkconfig.h"

#include "wifi.h"

#define WIFI_GOT_IP_BIT BIT0

static const char *const TAG = "wifi";

static EventGroupHandle_t wifi_events;
static esp_netif_t *sta_netif;
static esp_netif_t *ap_netif;
/* Reconnect on disconnection unless the probe gave up and switched to AP mode */
static bool sta_reconnect = true;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
	(void)arg;
	if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
		esp_wifi_connect();
	} else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
		xEventGroupClearBits(wifi_events, WIFI_GOT_IP_BIT);
		if (sta_reconnect) {
			const wifi_event_sta_disconnected_t *const event = data;
			ESP_LOGW(TAG, "disconnected (reason %d), reconnecting", event->reason);
			esp_wifi_connect();
		}
	} else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
		const ip_event_got_ip_t *const event = data;
		ESP_LOGI(TAG, "station IP " IPSTR, IP2STR(&event->ip_info.ip));
		xEventGroupSetBits(wifi_events, WIFI_GOT_IP_BIT);
	} else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
		ESP_LOGI(TAG, "client joined the access point");
	}
}

static bool wifi_start_station(void)
{
	wifi_config_t config = {0};
	strlcpy((char *)config.sta.ssid, CONFIG_MVPROBE_WIFI_SSID, sizeof(config.sta.ssid));
	strlcpy((char *)config.sta.password, CONFIG_MVPROBE_WIFI_PASSWORD, sizeof(config.sta.password));

	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
	ESP_ERROR_CHECK(esp_wifi_start());
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

	ESP_LOGI(TAG, "connecting to \"%s\"", CONFIG_MVPROBE_WIFI_SSID);
	const EventBits_t bits = xEventGroupWaitBits(
		wifi_events, WIFI_GOT_IP_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(CONFIG_MVPROBE_WIFI_STA_TIMEOUT_MS));
	if (bits & WIFI_GOT_IP_BIT)
		return true;

	ESP_LOGW(TAG, "no IP address after %d ms", CONFIG_MVPROBE_WIFI_STA_TIMEOUT_MS);
	sta_reconnect = false;
	ESP_ERROR_CHECK(esp_wifi_stop());
	return false;
}

static void wifi_start_access_point(void)
{
	wifi_config_t config = {
		.ap = {
			.channel = CONFIG_MVPROBE_AP_CHANNEL,
			.max_connection = 2,
			.authmode = WIFI_AUTH_WPA2_PSK,
		},
	};
	strlcpy((char *)config.ap.ssid, CONFIG_MVPROBE_AP_SSID, sizeof(config.ap.ssid));
	config.ap.ssid_len = strlen((const char *)config.ap.ssid);
	strlcpy((char *)config.ap.password, CONFIG_MVPROBE_AP_PASSWORD, sizeof(config.ap.password));
	if (strlen(CONFIG_MVPROBE_AP_PASSWORD) < 8U) {
		ESP_LOGW(TAG, "AP password shorter than 8 characters, starting an OPEN access point");
		config.ap.authmode = WIFI_AUTH_OPEN;
		config.ap.password[0] = '\0';
	}

	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
	ESP_ERROR_CHECK(esp_wifi_start());
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

	esp_netif_ip_info_t ip_info;
	esp_netif_get_ip_info(ap_netif, &ip_info);
	ESP_LOGI(TAG, "access point \"%s\" up, IP " IPSTR, CONFIG_MVPROBE_AP_SSID, IP2STR(&ip_info.ip));
}

static void mdns_start(void)
{
	ESP_ERROR_CHECK(mdns_init());
	ESP_ERROR_CHECK(mdns_hostname_set(CONFIG_MVPROBE_HOSTNAME));
	ESP_ERROR_CHECK(mdns_instance_name_set("MVProbe"));
	ESP_ERROR_CHECK(mdns_service_add("MVProbe GDB server", "_gdb", "_tcp", CONFIG_MVPROBE_GDB_PORT, NULL, 0));
	ESP_LOGI(TAG, "mDNS hostname %s.local", CONFIG_MVPROBE_HOSTNAME);
}

void wifi_start(void)
{
	wifi_events = xEventGroupCreate();

	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	sta_netif = esp_netif_create_default_wifi_sta();
	ap_netif = esp_netif_create_default_wifi_ap();
	ESP_ERROR_CHECK(esp_netif_set_hostname(sta_netif, CONFIG_MVPROBE_HOSTNAME));

	const wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&init_config));
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
	ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

	if (strlen(CONFIG_MVPROBE_WIFI_SSID) == 0U || !wifi_start_station())
		wifi_start_access_point();

	mdns_start();
}
