/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: MIT
 */

#include "Dongle.hpp"

#include <inttypes.h>
#include <string.h>

#include <algorithm>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"

namespace
{
constexpr const char* tag = "dongle";
constexpr uint8_t broadcastMac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
/* As on the probe: 16-QAM, about 2 MB/s with a margin (Tools/espnow_bench/RESULTS.md); broadcasts keep 1 Mb/s, for range */
constexpr wifi_phy_mode_t linkMode = WIFI_PHY_MODE_HT20;
constexpr wifi_phy_rate_t linkRate = WIFI_PHY_RATE_MCS3_LGI;
constexpr int sendTries = 20; /**< While esp_now_send() is out of buffers, a tick apart. */
constexpr uint32_t ledTaskStackSize = 3072;
constexpr uint32_t ledTaskPriority = 2;
constexpr uint32_t ledPeriodMs = 50;
constexpr uint32_t ledAlertTicks = 6;
constexpr uint8_t ledBrightness = 48;

bool isRequest(const NetworkProbe::Type type)
{
	return type < NetworkProbe::Type::RESP;
}

bool frameCrcMatches(const uint8_t* const frame, const uint16_t payloadLength)
{
	return NetworkProbe::getU32(frame + NetworkProbe::headerSize + payloadLength) == esp_rom_crc32_le(0, frame, NetworkProbe::headerSize + payloadLength);
}
}  // namespace

Dongle* Dongle::instance = nullptr;

void Dongle::start()
{
	instance = this;
	freeQueue = xQueueCreate(poolSize, sizeof(Packet*));
	rxQueue = xQueueCreate(poolSize, sizeof(Packet*));
	for (Packet& packet : pool)
	{
		Packet* const pointer = &packet;
		xQueueSend(freeQueue, &pointer, 0);
	}

	startLed();
	/* Before the callbacks, which wake it */
	xTaskCreatePinnedToCore(taskEntry, "dongle", taskStackSize, this, taskPriority, &task, taskCore);
	startWifi();
	startUsb();
}

void Dongle::taskEntry(void* const arg)
{
	static_cast<Dongle*>(arg)->run();
}

void Dongle::ledTaskEntry(void* const arg)
{
	static_cast<Dongle*>(arg)->runLed();
}

void Dongle::onReceive(const esp_now_recv_info_t* const info, const uint8_t* const data, const int length)
{
	/* Wi-Fi task: copy and hand over */
	Dongle& self = *instance;
	if (length < static_cast<int>(EspNowLink::headerSize) || length > static_cast<int>(EspNowLink::maxPacket))
		return;
	Packet* packet = nullptr;
	if (xQueueReceive(self.freeQueue, &packet, 0) != pdTRUE)
	{
		self.radioDrops.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	memcpy(packet->mac, info->src_addr, sizeof(packet->mac));
	packet->length = static_cast<uint16_t>(length);
	memcpy(packet->data, data, length);
	xQueueSend(self.rxQueue, &packet, 0);
	xTaskNotifyGive(self.task);
}

void Dongle::onUsbReceive(const int itf, cdcacm_event_t* const event)
{
	xTaskNotifyGive(instance->task);
}

void Dongle::onLineState(const int itf, cdcacm_event_t* const event)
{
	/* A host sends the line state again whenever it reconfigures the port: only a change of DTR opens or closes */
	const bool dtr = event->line_state_changed_data.dtr;
	if (instance->hostOpen.exchange(dtr) == dtr)
		return;
	instance->lineChanged = true;
	xTaskNotifyGive(instance->task);
}

void Dongle::run()
{
	uint32_t reportedRadioDrops = 0;
	uint32_t reportedHostDrops = 0;
	while (true)
	{
		ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(pollMs));
		serviceLine();
		readHost();

		Packet* packet = nullptr;
		while (xQueueReceive(rxQueue, &packet, 0) == pdTRUE)
		{
			handlePacket(*packet);
			xQueueSend(freeQueue, &packet, 0);
		}

		if (selected && esp_timer_get_time() - lastSendUs >= EspNowLink::pingIntervalMs * 1000LL)
			sendToProbe(EspNowLink::Kind::PING, nullptr, 0);
		tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);

		const uint32_t radioDropped = radioDrops.load(std::memory_order_relaxed);
		if (radioDropped != reportedRadioDrops || hostDrops != reportedHostDrops)
		{
			ESP_LOGW(tag, "dropped so far: %" PRIu32 " packets from the radio, %" PRIu32 " frames for the host", radioDropped, hostDrops);
			reportedRadioDrops = radioDropped;
			reportedHostDrops = hostDrops;
		}
	}
}

void Dongle::runLed()
{
	uint32_t seen = 0;
	uint32_t alertLeft = 0;
	for (uint32_t tick = 0;; ++tick)
	{
		if (alert.exchange(false))
			alertLeft = ledAlertTicks;
		const uint32_t count = traffic.load(std::memory_order_relaxed);
		uint8_t red = 0;
		uint8_t green = 0;
		uint8_t blue = 0;
		if (alertLeft)
		{
			alertLeft--;
			red = ledBrightness;
		}
		else if (selected)
			green = count != seen ? ledBrightness / 6 : ledBrightness;
		else
			blue = tick % 20 < 2 ? ledBrightness : 0;
		seen = count;
		led_strip_set_pixel(led, 0, red, green, blue);
		led_strip_refresh(led);
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

	/* A station that joins nothing: ESP-NOW only */
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	const wifi_init_config_t initConfig = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&initConfig));
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_start());
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
	ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_DONGLE_CHANNEL, WIFI_SECOND_CHAN_NONE));

	ESP_ERROR_CHECK(esp_now_init());
	ESP_ERROR_CHECK(esp_now_register_recv_cb(onReceive));
	esp_now_peer_info_t peer = {};
	memcpy(peer.peer_addr, broadcastMac, sizeof(peer.peer_addr));
	peer.ifidx = WIFI_IF_STA;
	ESP_ERROR_CHECK(esp_now_add_peer(&peer));

	uint8_t mac[6];
	esp_read_mac(mac, ESP_MAC_WIFI_STA);
	ESP_LOGI(tag, "ESP-NOW on channel %d as " MACSTR, CONFIG_DONGLE_CHANNEL, MAC2STR(mac));
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

	tinyusb_config_cdcacm_t cdcConfig = {};
	cdcConfig.cdc_port = TINYUSB_CDC_ACM_0;
	cdcConfig.callback_rx = onUsbReceive;
	cdcConfig.callback_line_state_changed = onLineState;
	ESP_ERROR_CHECK(tinyusb_cdcacm_init(&cdcConfig));
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
		return;
	}
	xTaskCreate(ledTaskEntry, "led", ledTaskStackSize, this, ledTaskPriority, nullptr);
}

void Dongle::serviceLine()
{
	if (!lineChanged.exchange(false))
		return;
	if (hostOpen)
	{
		/* What waited for a host that went away is stale */
		tud_cdc_n_write_clear(0);
		hostLength = 0;
		hostSkip = 0;
		ESP_LOGI(tag, "the host opened the port");
	}
	else
	{
		ESP_LOGI(tag, "the host closed the port");
		deselect(true);
	}
}

void Dongle::readHost()
{
	while (hostLength < sizeof(hostBuffer))
	{
		size_t received = 0;
		if (tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, hostBuffer + hostLength, sizeof(hostBuffer) - hostLength, &received) != ESP_OK || !received)
			return;
		hostLength += received;
		parseHost();
	}
}

void Dongle::parseHost()
{
	/* A byte stream as over TCP; after a corrupt frame, the next magic starts over */
	size_t offset = 0;
	while (offset < hostLength)
	{
		if (hostSkip)
		{
			const size_t skipped = std::min(hostSkip, hostLength - offset);
			offset += skipped;
			hostSkip -= skipped;
			continue;
		}
		if (hostLength - offset < NetworkProbe::headerSize)
			break;

		const uint8_t* const frame = hostBuffer + offset;
		NetworkProbe::FrameHeader header;
		if (!NetworkProbe::decodeHeader(frame, header))
		{
			offset++;
			continue;
		}
		const size_t size = NetworkProbe::frameOverhead + header.length;
		if (size > EspNowLink::maxFrame)
		{
			/* It cannot travel in one packet: the host ignored max_payload */
			ESP_LOGW(tag, "frame of %u bytes from the host exceeds %u, skipped", static_cast<unsigned>(size), static_cast<unsigned>(EspNowLink::maxFrame));
			if (isRequest(header.type))
				answerHost(header.seq, header.type, NetworkProbe::Status::MALFORMED);
			hostSkip = size;
			continue;
		}
		if (hostLength - offset < size)
			break;
		if (!frameCrcMatches(frame, header.length))
		{
			offset++;
			continue;
		}
		handleHostFrame(header, frame, size);
		offset += size;
	}

	memmove(hostBuffer, hostBuffer + offset, hostLength - offset);
	hostLength -= offset;
}

void Dongle::handleHostFrame(const NetworkProbe::FrameHeader& header, const uint8_t* const frame, const size_t length)
{
	switch (header.type)
	{
		case NetworkProbe::Type::DISCOVER:
		case NetworkProbe::Type::IDENTIFY:
			/* Every probe in range hears it; the answers come back as packets */
			sendPacket(broadcastMac, EspNowLink::Kind::DATA, EspNowLink::noSession, frame, length);
			return;
		case NetworkProbe::Type::DONGLE_SELECT:
			handleSelect(header, frame + NetworkProbe::headerSize);
			return;
		default:
			break;
	}
	if (!isRequest(header.type))
		return;
	if (!selected)
		answerHost(header.seq, header.type, NetworkProbe::Status::NO_PROBE);
	else if (!sendToProbe(EspNowLink::Kind::DATA, frame, length))
		ESP_LOGW(tag, "a request of type 0x%02x did not go out", NetworkProbe::toByte(header.type));
}

void Dongle::handleSelect(const NetworkProbe::FrameHeader& header, const uint8_t* const payload)
{
	if (header.length < 1 || header.length != 1U + payload[0])
	{
		answerHost(header.seq, header.type, NetworkProbe::Status::MALFORMED);
		return;
	}
	for (const Probe& probe : probes)
	{
		if (probe.used && strlen(probe.id) == payload[0] && !memcmp(probe.id, payload + 1, payload[0]))
		{
			select(probe);
			answerHost(header.seq, header.type, NetworkProbe::Status::OK);
			return;
		}
	}
	answerHost(header.seq, header.type, NetworkProbe::Status::NO_PROBE);
}

void Dongle::answerHost(const uint16_t seq, const NetworkProbe::Type request, const NetworkProbe::Status status)
{
	uint8_t frame[NetworkProbe::frameOverhead + NetworkProbe::respHeaderSize];
	NetworkProbe::encodeHeader(frame, NetworkProbe::Type::RESP, NetworkProbe::respHeaderSize, seq);
	frame[NetworkProbe::headerSize] = NetworkProbe::toByte(request);
	frame[NetworkProbe::headerSize + 1] = NetworkProbe::toByte(status);
	writeHost(frame, NetworkProbe::sealFrame(frame, NetworkProbe::respHeaderSize));
}

void Dongle::writeHost(const uint8_t* const frame, const size_t length)
{
	/* A frame goes whole or not at all, so the host's stream stays in step */
	if (tud_cdc_n_write_available(0) < length)
	{
		if (hostOpen)
			tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, hostFlushTimeout);
		if (tud_cdc_n_write_available(0) < length)
		{
			hostDrops++;
			return;
		}
	}
	tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, frame, length);
	traffic.fetch_add(1, std::memory_order_relaxed);
}

void Dongle::handlePacket(const Packet& packet)
{
	EspNowLink::Header linkHeader;
	if (!EspNowLink::decodeHeader(packet.data, packet.length, linkHeader))
		return;
	const bool fromSession = selected && linkHeader.session == session && !memcmp(packet.mac, probeMac, sizeof(probeMac));

	if (linkHeader.kind == EspNowLink::Kind::CLOSE)
	{
		if (fromSession)
		{
			/* Another client took over, or the probe heard nothing for too long: requests get NO_PROBE from now on */
			ESP_LOGW(tag, "the probe ended session %u", session);
			deselect(false);
			alert = true;
		}
		return;
	}
	if (linkHeader.kind != EspNowLink::Kind::DATA)
		return;

	const uint8_t* const frame = packet.data + EspNowLink::headerSize;
	const size_t length = packet.length - EspNowLink::headerSize;
	if (linkHeader.session == EspNowLink::noSession)
	{
		rememberProbe(packet.mac, frame, length);
		writeHost(frame, length);
	}
	else if (fromSession)
		writeHost(frame, length);
}

void Dongle::rememberProbe(const uint8_t* const mac, const uint8_t* const frame, const size_t length)
{
	NetworkProbe::FrameHeader header;
	if (length < NetworkProbe::frameOverhead || !NetworkProbe::decodeHeader(frame, header) || length != NetworkProbe::frameOverhead + header.length || !frameCrcMatches(frame, header.length))
		return;
	const uint8_t* const payload = frame + NetworkProbe::headerSize;
	const uint8_t* const end = payload + header.length;
	if (header.type != NetworkProbe::Type::RESP || header.length < NetworkProbe::respHeaderSize + NetworkProbe::discoverResponseFixedSize || payload[0] != NetworkProbe::toByte(NetworkProbe::Type::DISCOVER) ||
		payload[1] != NetworkProbe::toByte(NetworkProbe::Status::OK))
		return;

	/* After the fixed fields: name, firmware, id and hostname, each a u8 length, then the bytes */
	const uint8_t* field = payload + NetworkProbe::respHeaderSize + NetworkProbe::discoverResponseFixedSize;
	for (int skipped = 0; skipped < 2 && field < end; ++skipped)
		field += 1 + *field;
	if (field >= end || field + 1 + *field > end || *field >= sizeof(Probe::id))
		return;

	char id[sizeof(Probe::id)];
	memcpy(id, field + 1, *field);
	id[*field] = '\0';

	Probe* slot = nullptr;
	for (Probe& probe : probes)
		if (probe.used && !strcmp(probe.id, id))
			slot = &probe;
	for (Probe& probe : probes)
		if (!slot && !probe.used)
			slot = &probe;
	if (!slot)
	{
		slot = &probes[nextProbe];
		nextProbe = (nextProbe + 1) % maxProbes;
	}
	strlcpy(slot->id, id, sizeof(slot->id));
	memcpy(slot->mac, mac, sizeof(slot->mac));
	slot->used = true;
}

void Dongle::select(const Probe& probe)
{
	deselect(true);
	memcpy(probeMac, probe.mac, sizeof(probeMac));

	esp_now_peer_info_t peer = {};
	memcpy(peer.peer_addr, probeMac, sizeof(peer.peer_addr));
	peer.ifidx = WIFI_IF_STA;
	esp_err_t result = esp_now_is_peer_exist(probeMac) ? ESP_OK : esp_now_add_peer(&peer);
	if (result == ESP_ERR_ESPNOW_FULL)
	{
		/* Probes selected before: every unicast peer goes, the broadcast one stays */
		esp_now_peer_info_t old = {};
		while (esp_now_fetch_peer(true, &old) == ESP_OK)
			esp_now_del_peer(old.peer_addr);
		result = esp_now_add_peer(&peer);
	}
	if (result == ESP_OK)
	{
		/* Without it ESP-NOW sends at 1 Mb/s */
		esp_now_rate_config_t rate = {};
		rate.phymode = linkMode;
		rate.rate = linkRate;
		result = esp_now_set_peer_rate_config(probeMac, &rate);
	}
	if (result != ESP_OK)
		ESP_LOGW(tag, "peer " MACSTR ": %s", MAC2STR(probeMac), esp_err_to_name(result));

	do
		session = static_cast<uint16_t>(esp_random());
	while (session == EspNowLink::noSession);
	lastSendUs = esp_timer_get_time();
	selected = true;
	ESP_LOGI(tag, "session %u with %s, " MACSTR, session, probe.id, MAC2STR(probeMac));
}

void Dongle::deselect(const bool tellProbe)
{
	if (!selected)
		return;
	if (tellProbe)
		sendToProbe(EspNowLink::Kind::CLOSE, nullptr, 0);
	selected = false;
}

bool Dongle::sendToProbe(const EspNowLink::Kind kind, const uint8_t* const frame, const size_t length)
{
	lastSendUs = esp_timer_get_time();
	return sendPacket(probeMac, kind, session, frame, length);
}

bool Dongle::sendPacket(const uint8_t* const mac, const EspNowLink::Kind kind, const uint16_t packetSession, const uint8_t* const frame, const size_t length)
{
	EspNowLink::encodeHeader(txPacket, kind, packetSession);
	if (length)
		memcpy(txPacket + EspNowLink::headerSize, frame, length);
	for (int attempt = 0; attempt < sendTries; ++attempt)
	{
		const esp_err_t result = esp_now_send(mac, txPacket, EspNowLink::headerSize + length);
		if (result == ESP_OK)
		{
			traffic.fetch_add(1, std::memory_order_relaxed);
			return true;
		}
		if (result != ESP_ERR_ESPNOW_NO_MEM)
		{
			ESP_LOGW(tag, "esp_now_send(): %s", esp_err_to_name(result));
			return false;
		}
		vTaskDelay(1);
	}
	return false;
}
