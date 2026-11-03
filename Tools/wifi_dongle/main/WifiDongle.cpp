/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: MIT
 *
 * Wi-Fi dongle demo: an ESP32-S3 on the PC's USB that joins a probe's access point and shows up on the PC as a network
 * adapter (USB CDC-NCM). Ethernet frames pass between USB and Wi-Fi unchanged: the adapter takes the station's MAC
 * address, so to the access point the PC's frames are the station's own. The PC gets its address from the probe and
 * reaches it at 192.168.4.1 like over any network, MCUViewer and GDB included, while it keeps its own Wi-Fi for the
 * internet. Built after ESP-IDF's tusb_ncm and sta2eth examples.
 */

#include <string.h>

#include <algorithm>
#include <atomic>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_private/wifi.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_net.h"

namespace
{
constexpr const char* tag = "wifi_dongle";
constexpr char probePrefix[] = "network-probe-"; /* A probe's access point: the prefix, then its device id */
constexpr bool namedNetwork = sizeof(CONFIG_DONGLE_WIFI_SSID) > 1;
constexpr bool hasPassword = sizeof(CONFIG_DONGLE_WIFI_PASSWORD) > 1;
constexpr uint16_t maxScanRecords = 32;
constexpr uint32_t joinTimeoutMs = 10000;
constexpr uint32_t retryDelayMs = 1000;
constexpr TickType_t usbSendTimeout = pdMS_TO_TICKS(100);
constexpr uint32_t taskStackSize = 4096;
constexpr uint32_t taskPriority = 5;
constexpr uint32_t ledTaskStackSize = 3072;
constexpr uint32_t ledTaskPriority = 2;
constexpr uint32_t ledPeriodMs = 50;
constexpr uint8_t ledBrightness = 48;
constexpr EventBits_t connectedBit = BIT0;
constexpr EventBits_t disconnectedBit = BIT1;

uint16_t getU16Be(const uint8_t* const in)
{
	return static_cast<uint16_t>(in[0] << 8U | in[1]);
}

/* DHCP options that would make the dongle the PC's way out: router, DNS server, domain name, routes, captive portal, proxy discovery */
bool isRouteOption(const uint8_t code)
{
	return code == 3 || code == 6 || code == 15 || code == 114 || code == 121 || code == 249 || code == 252;
}

/**
 * @brief Blanks the route options of a DHCP answer on its way to the PC, so the PC keeps its own network for the
 * internet and uses the dongle for the probe's subnet only. Every other frame passes untouched.
 */
void hideRoutes(uint8_t* const frame, const size_t length)
{
	constexpr size_t ethernetHeader = 14;
	constexpr size_t bootpFixed = 236; /* Up to the magic cookie */
	const uint8_t* const end = frame + length;
	if (length < ethernetHeader + 20 || getU16Be(frame + 12) != 0x0800)
		return;
	uint8_t* const ip = frame + ethernetHeader;
	const size_t ipHeader = (ip[0] & 0x0fU) * 4U;
	if ((ip[0] >> 4U) != 4 || ipHeader < 20 || ip[9] != 17 || (getU16Be(ip + 6) & 0x3fffU))
		return;
	uint8_t* const udp = ip + ipHeader;
	if (udp + 8 > end || getU16Be(udp) != 67 || getU16Be(udp + 2) != 68)
		return;
	const uint8_t* const udpEnd = std::min(end, static_cast<const uint8_t*>(udp + getU16Be(udp + 4)));
	uint8_t* option = udp + 8 + bootpFixed;
	if (option + 4 > udpEnd || option[0] != 99 || option[1] != 130 || option[2] != 83 || option[3] != 99)
		return;

	bool changed = false;
	for (option += 4; option < udpEnd && *option != 255;)
	{
		if (!*option)
		{
			option++; /* Pad */
			continue;
		}
		if (option + 2 > udpEnd || option + 2 + option[1] > udpEnd)
			break;
		const size_t size = 2U + option[1];
		if (isRouteOption(option[0]))
		{
			memset(option, 0, size); /* Pads */
			changed = true;
		}
		option += size;
	}
	/* No UDP checksum then, which IPv4 allows */
	if (changed)
	{
		udp[6] = 0;
		udp[7] = 0;
	}
}

/**
 * @brief Looks for the network, joins it, and forwards frames between it and the PC until the link drops; then looks
 * again. The status LED blinks blue while it looks and shows green once joined, dipping for the traffic.
 */
class Dongle
{
   public:
	void start();

   private:
	static void taskEntry(void* arg);
	static void ledTaskEntry(void* arg);
	static void onEvent(void* arg, esp_event_base_t base, int32_t id, void* data);
	static esp_err_t fromUsb(void* buffer, uint16_t length, void* context);
	static esp_err_t fromWifi(void* buffer, uint16_t length, void* eb);
	static void freeWifiBuffer(void* eb, void* context);

	[[noreturn]] void run();
	[[noreturn]] void runLed();
	void startWifi();
	void startUsb();
	void startLed();

	/**
	 * @brief Scans for the configured network, or else for the strongest probe access point.
	 */
	bool findNetwork(wifi_ap_record_t& target);
	bool join(const wifi_ap_record_t& target);
	void showColor(uint8_t red, uint8_t green, uint8_t blue);

	static Dongle* instance;

	EventGroupHandle_t events = nullptr;
	std::atomic<bool> linked{false};
	std::atomic<uint32_t> frames{0}; /* Forwarded, both ways, for the LED */
	led_strip_handle_t led = nullptr;
	bool passwordWarned = false;
	wifi_ap_record_t records[maxScanRecords];
};

Dongle* Dongle::instance = nullptr;

void Dongle::start()
{
	instance = this;
	events = xEventGroupCreate();
	startLed();
	startWifi();
	startUsb();
	xTaskCreate(taskEntry, "dongle", taskStackSize, this, taskPriority, nullptr);
}

void Dongle::taskEntry(void* const arg)
{
	static_cast<Dongle*>(arg)->run();
}

void Dongle::ledTaskEntry(void* const arg)
{
	static_cast<Dongle*>(arg)->runLed();
}

void Dongle::onEvent(void* const arg, const esp_event_base_t base, const int32_t id, void* const data)
{
	Dongle& self = *static_cast<Dongle*>(arg);
	if (id == WIFI_EVENT_STA_CONNECTED)
	{
		esp_wifi_internal_reg_rxcb(WIFI_IF_STA, fromWifi);
		self.linked = true;
		/* The cable plugged in, as the PC sees it: it asks for an address now */
		tud_network_link_state(0, true);
		xEventGroupSetBits(self.events, connectedBit);
	}
	else if (id == WIFI_EVENT_STA_DISCONNECTED)
	{
		esp_wifi_internal_reg_rxcb(WIFI_IF_STA, nullptr);
		const uint8_t reason = static_cast<const wifi_event_sta_disconnected_t*>(data)->reason;
		if (self.linked.exchange(false))
			ESP_LOGW(tag, "link lost, reason %u", reason);
		else
			ESP_LOGW(tag, "could not join, reason %u", reason);
		tud_network_link_state(0, false);
		xEventGroupSetBits(self.events, disconnectedBit);
	}
}

esp_err_t Dongle::fromUsb(void* const buffer, const uint16_t length, void* const context)
{
	Dongle& self = *instance;
	if (self.linked.load(std::memory_order_relaxed))
	{
		esp_wifi_internal_tx(WIFI_IF_STA, buffer, length);
		self.frames.fetch_add(1, std::memory_order_relaxed);
	}
	return ESP_OK;
}

esp_err_t Dongle::fromWifi(void* const buffer, const uint16_t length, void* const eb)
{
	Dongle& self = *instance;
	hideRoutes(static_cast<uint8_t*>(buffer), length);
	/* Once TinyUSB has copied the frame, it gives the Wi-Fi buffer back through freeWifiBuffer() */
	if (tinyusb_net_send_sync(buffer, length, eb, usbSendTimeout) == ESP_OK)
		self.frames.fetch_add(1, std::memory_order_relaxed);
	else
		esp_wifi_internal_free_rx_buffer(eb);
	return ESP_OK;
}

void Dongle::freeWifiBuffer(void* const eb, void* const context)
{
	esp_wifi_internal_free_rx_buffer(eb);
}

void Dongle::run()
{
	uint8_t mac[6];
	esp_read_mac(mac, ESP_MAC_WIFI_STA);
	ESP_LOGI(tag, "USB network adapter " MACSTR ", looking for %s", MAC2STR(mac), namedNetwork ? CONFIG_DONGLE_WIFI_SSID : "a probe access point");

	bool reportedNone = false;
	while (true)
	{
		wifi_ap_record_t target;
		if (!findNetwork(target))
		{
			if (!reportedNone)
				ESP_LOGI(tag, "none in range yet, still looking");
			reportedNone = true;
			vTaskDelay(pdMS_TO_TICKS(retryDelayMs));
			continue;
		}
		reportedNone = false;
		if (!join(target))
		{
			vTaskDelay(pdMS_TO_TICKS(retryDelayMs));
			continue;
		}

		/* Until the link drops; then look again, as the probe may have moved, restarted or closed its access point */
		xEventGroupWaitBits(events, disconnectedBit, pdTRUE, pdTRUE, portMAX_DELAY);
	}
}

void Dongle::runLed()
{
	uint32_t seen = 0;
	for (uint32_t tick = 0;; ++tick)
	{
		const uint32_t count = frames.load(std::memory_order_relaxed);
		if (linked)
			showColor(0, count != seen ? ledBrightness / 6 : ledBrightness, 0);
		else
			showColor(0, 0, tick % 20 < 2 ? ledBrightness : 0);
		seen = count;
		vTaskDelay(pdMS_TO_TICKS(ledPeriodMs));
	}
}

void Dongle::startWifi()
{
	esp_err_t result = nvs_flash_init();
	if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND)
	{
		ESP_ERROR_CHECK(nvs_flash_erase());
		result = nvs_flash_init();
	}
	ESP_ERROR_CHECK(result);

	/* No network interface of its own: the PC's IP stack is the one on this link */
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	const wifi_init_config_t initConfig = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&initConfig));
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, onEvent, this));
	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, onEvent, this));
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_start());
	/* Modem sleep would hold the frames for the PC until the next beacon */
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

void Dongle::startUsb()
{
	tinyusb_config_t usbConfig = {};
	usbConfig.port = TINYUSB_PORT_FULL_SPEED_0;
	usbConfig.phy.vbus_monitor_io = -1;
	usbConfig.task.size = TINYUSB_DEFAULT_TASK_SIZE;
	usbConfig.task.priority = TINYUSB_DEFAULT_TASK_PRIO;
	usbConfig.task.xCoreID = TINYUSB_DEFAULT_TASK_AFFINITY;
	ESP_ERROR_CHECK(tinyusb_driver_install(&usbConfig));

	/* The adapter takes the station's MAC address, so the access point takes the PC's frames for the station's */
	tinyusb_net_config_t netConfig = {};
	esp_read_mac(netConfig.mac_addr, ESP_MAC_WIFI_STA);
	netConfig.on_recv_callback = fromUsb;
	netConfig.free_tx_buffer = freeWifiBuffer;
	ESP_ERROR_CHECK(tinyusb_net_init(&netConfig));
	tud_network_link_state(0, false);
}

void Dongle::startLed()
{
	if (CONFIG_DONGLE_LED_GPIO < 0)
		return;

	led_strip_config_t config = {};
	config.strip_gpio_num = CONFIG_DONGLE_LED_GPIO;
	config.max_leds = 1;
	config.led_model = LED_MODEL_WS2812;
#if CONFIG_DONGLE_LED_ORDER_RGB
	config.color_component_format.format.r_pos = 0;
	config.color_component_format.format.g_pos = 1;
#else
	config.color_component_format.format.r_pos = 1;
	config.color_component_format.format.g_pos = 0;
#endif
	config.color_component_format.format.b_pos = 2;
	config.color_component_format.format.w_pos = 3;
	config.color_component_format.format.num_components = 3;
	const led_strip_rmt_config_t rmtConfig = {};
	if (led_strip_new_rmt_device(&config, &rmtConfig, &led) != ESP_OK)
	{
		ESP_LOGW(tag, "no status LED on GPIO %d", CONFIG_DONGLE_LED_GPIO);
		led = nullptr;
		return;
	}
	xTaskCreate(ledTaskEntry, "led", ledTaskStackSize, this, ledTaskPriority, nullptr);
}

bool Dongle::findNetwork(wifi_ap_record_t& target)
{
	const wifi_scan_config_t scan = {};
	if (esp_wifi_scan_start(&scan, true) != ESP_OK)
		return false;
	uint16_t count = maxScanRecords;
	if (esp_wifi_scan_get_ap_records(&count, records) != ESP_OK)
		return false;

	const wifi_ap_record_t* best = nullptr;
	for (uint16_t i = 0; i < count; ++i)
	{
		const wifi_ap_record_t& record = records[i];
		const char* const ssid = reinterpret_cast<const char*>(record.ssid);
		const bool matches = namedNetwork ? !strcmp(ssid, CONFIG_DONGLE_WIFI_SSID) : !strncmp(ssid, probePrefix, sizeof(probePrefix) - 1);
		if (!matches)
			continue;
		if (record.authmode != WIFI_AUTH_OPEN && !hasPassword)
		{
			if (!passwordWarned)
				ESP_LOGW(tag, "%s needs a password: set CONFIG_DONGLE_WIFI_PASSWORD", ssid);
			passwordWarned = true;
			continue;
		}
		if (!best || record.rssi > best->rssi)
			best = &record;
	}
	if (!best)
		return false;
	target = *best;
	return true;
}

bool Dongle::join(const wifi_ap_record_t& target)
{
	const char* const ssid = reinterpret_cast<const char*>(target.ssid);
	wifi_config_t config = {};
	memcpy(config.sta.ssid, target.ssid, sizeof(config.sta.ssid));
	strlcpy(reinterpret_cast<char*>(config.sta.password), CONFIG_DONGLE_WIFI_PASSWORD, sizeof(config.sta.password));
	/* That very access point, on its channel: no second scan */
	memcpy(config.sta.bssid, target.bssid, sizeof(config.sta.bssid));
	config.sta.bssid_set = true;
	config.sta.channel = target.primary;
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));

	ESP_LOGI(tag, "joining %s on channel %u, RSSI %d", ssid, target.primary, target.rssi);
	xEventGroupClearBits(events, connectedBit | disconnectedBit);
	esp_wifi_connect();
	const EventBits_t bits = xEventGroupWaitBits(events, connectedBit | disconnectedBit, pdFALSE, pdFALSE, pdMS_TO_TICKS(joinTimeoutMs));
	if (!(bits & connectedBit))
	{
		esp_wifi_disconnect();
		return false;
	}
	ESP_LOGI(tag, "joined %s%s", ssid, namedNetwork ? "" : ": the probe answers at 192.168.4.1");
	return true;
}

void Dongle::showColor(const uint8_t red, const uint8_t green, const uint8_t blue)
{
	led_strip_set_pixel(led, 0, red, green, blue);
	led_strip_refresh(led);
}

Dongle dongle;
}  // namespace

extern "C" void app_main()
{
	dongle.start();
}
