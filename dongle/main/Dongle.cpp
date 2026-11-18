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
		releasePacket(&packet);

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
	packet->rssi = static_cast<int8_t>(info->rx_ctrl->rssi);
	packet->channel = static_cast<uint8_t>(info->rx_ctrl->channel);
	packet->length = static_cast<uint16_t>(length);
	memcpy(packet->data, data, length);
	xQueueSend(self.rxQueue, &packet, 0);
	xTaskNotifyGive(self.task);
}

void Dongle::onSent(const esp_now_send_info_t* const info, const esp_now_send_status_t status)
{
	Dongle& self = *instance;
	self.txEndUs = esp_timer_get_time();
	if (status == ESP_NOW_SEND_SUCCESS)
	{
		if (info && info->des_addr && !memcmp(info->des_addr, self.probeMac, sizeof(self.probeMac)))
			self.lastAckUs = esp_timer_get_time();
		self.txState = TxState::idle;
	}
	else
		self.txState = TxState::failed;
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
	uint32_t reportedLost = 0;
	while (true)
	{
		ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(hostQueueCount || sweepChannel ? busyPollMs : pollMs));
		serviceLine();
		readHost();

		Packet* packet = nullptr;
		while (xQueueReceive(rxQueue, &packet, 0) == pdTRUE)
			handlePacket(packet);
		flushHost();
		serviceSession();
		serviceSweep();
		serviceTx();
		tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);

		const uint32_t radioDropped = radioDrops.load(std::memory_order_relaxed);
		if (radioDropped != reportedRadioDrops || hostDrops != reportedHostDrops || txLost != reportedLost)
		{
			ESP_LOGW(tag, "so far: %" PRIu32 " packets from the radio dropped, %" PRIu32 " frames for the host dropped, %" PRIu32 " packets lost on the air", radioDropped, hostDrops, txLost);
			reportedRadioDrops = radioDropped;
			reportedHostDrops = hostDrops;
			reportedLost = txLost;
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

	/* A station that joins nothing: ESP-NOW only, on the channels the country allows, at every rung of the ladder */
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	const wifi_init_config_t initConfig = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&initConfig));
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	ESP_ERROR_CHECK(esp_wifi_set_country_code(CONFIG_DONGLE_COUNTRY, true));
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_start());
	ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, EspNowLink::protocols));
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

	wifi_country_t country = {};
	ESP_ERROR_CHECK(esp_wifi_get_country(&country));
	firstChannel = country.schan;
	lastChannel = static_cast<uint8_t>(country.schan + country.nchan - 1);
	setChannel(firstChannel);

	ESP_ERROR_CHECK(esp_now_init());
	ESP_ERROR_CHECK(esp_now_register_recv_cb(onReceive));
	ESP_ERROR_CHECK(esp_now_register_send_cb(onSent));
	esp_now_peer_info_t peer = {};
	memcpy(peer.peer_addr, broadcastMac, sizeof(peer.peer_addr));
	peer.ifidx = WIFI_IF_STA;
	ESP_ERROR_CHECK(esp_now_add_peer(&peer));

	uint8_t mac[6];
	esp_read_mac(mac, ESP_MAC_WIFI_STA);
	ESP_LOGI(tag, "ESP-NOW on channels %u to %u (country %s) as " MACSTR, firstChannel, lastChannel, CONFIG_DONGLE_COUNTRY, MAC2STR(mac));
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
		/* Nobody reads what waits for the host any more */
		while (hostQueueCount)
		{
			releasePacket(hostQueue[hostQueueHead]);
			hostQueueHead = (hostQueueHead + 1) % poolSize;
			hostQueueCount--;
		}
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
			/* Every probe in range hears it; the answers come back as packets. The session owns the radio */
			answeredCount = 0;
			if (selected)
				queuePacket(broadcastMac, probeChannel, {EspNowLink::Kind::DATA, EspNowLink::noSession, 0, 0, 0}, frame, length);
			else
				startSweep(frame, length);
			return;
		case NetworkProbe::Type::DONGLE_SELECT:
			handleSelect(header, frame + NetworkProbe::headerSize);
			return;
		case NetworkProbe::Type::DONGLE_STATUS:
			handleStatus(header);
			return;
		default:
			break;
	}
	if (!isRequest(header.type))
		return;
	if (!selected)
		answerHost(header.seq, header.type, NetworkProbe::Status::NO_PROBE);
	else if (!sendToProbe(EspNowLink::Kind::DATA, frame, length))
		ESP_LOGW(tag, "no room to send a request of type 0x%02x", NetworkProbe::toByte(header.type));
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

void Dongle::handleStatus(const NetworkProbe::FrameHeader& header)
{
	if (header.length != 0)
	{
		answerHost(header.seq, header.type, NetworkProbe::Status::MALFORMED);
		return;
	}
	if (!selected)
	{
		answerHost(header.seq, header.type, NetworkProbe::Status::NO_PROBE);
		return;
	}
	const uint8_t data[NetworkProbe::dongleStatusResponseSize] = {EspNowLink::rates[ladder.getRung()].halfMbps, static_cast<uint8_t>(probeRssi), probeChannel};
	answerHost(header.seq, header.type, NetworkProbe::Status::OK, data, sizeof(data));
}

void Dongle::answerHost(const uint16_t seq, const NetworkProbe::Type request, const NetworkProbe::Status status, const uint8_t* const data, const size_t dataLength)
{
	uint8_t frame[NetworkProbe::frameOverhead + NetworkProbe::respHeaderSize + NetworkProbe::dongleStatusResponseSize];
	const uint16_t payloadLength = static_cast<uint16_t>(NetworkProbe::respHeaderSize + dataLength);
	NetworkProbe::encodeHeader(frame, NetworkProbe::Type::RESP, payloadLength, seq);
	frame[NetworkProbe::headerSize] = NetworkProbe::toByte(request);
	frame[NetworkProbe::headerSize + 1] = NetworkProbe::toByte(status);
	if (dataLength)
		memcpy(frame + NetworkProbe::headerSize + NetworkProbe::respHeaderSize, data, dataLength);
	const size_t length = NetworkProbe::sealFrame(frame, payloadLength);
	if (!writeHost(frame, length) && hostOpen)
	{
		/* Small and rare: worth a short wait for room */
		tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, answerFlushTimeout);
		if (!writeHost(frame, length))
			hostDrops++;
	}
}

bool Dongle::writeHost(const uint8_t* const frame, const size_t length)
{
	/* A frame goes whole or not at all, so the host's stream stays in step */
	if (tud_cdc_n_write_available(0) < length)
		return false;
	tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, frame, length);
	traffic.fetch_add(1, std::memory_order_relaxed);
	return true;
}

void Dongle::flushHost()
{
	while (hostQueueCount)
	{
		Packet* const packet = hostQueue[hostQueueHead];
		if (!writeHost(packet->data + EspNowLink::headerSize, packet->length - EspNowLink::headerSize))
		{
			/* Starts the USB transfer of what is queued, so room comes */
			tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
			return;
		}
		releasePacket(packet);
		hostQueueHead = (hostQueueHead + 1) % poolSize;
		hostQueueCount--;
	}
}

void Dongle::handlePacket(Packet* const packet)
{
	EspNowLink::Header linkHeader;
	if (!EspNowLink::decodeHeader(packet->data, packet->length, linkHeader))
	{
		releasePacket(packet);
		return;
	}
	const bool fromProbe = selected && !memcmp(packet->mac, probeMac, sizeof(probeMac));
	const bool fromSession = fromProbe && linkHeader.session == session;
	if (fromProbe)
	{
		probeRssi = packet->rssi;
		/* The probe is there: the packet waiting for it goes now */
		if (txState == TxState::quiet)
			quietUntilUs = 0;
	}

	if (linkHeader.kind == EspNowLink::Kind::CLOSE && fromSession)
	{
		/* Another client took over, or the probe heard nothing for too long: requests get NO_PROBE from now on */
		ESP_LOGW(tag, "the probe ended session %u", session);
		deselect(false);
		alert = true;
	}
	if (linkHeader.kind != EspNowLink::Kind::DATA)
	{
		releasePacket(packet);
		return;
	}

	const uint8_t* const frame = packet->data + EspNowLink::headerSize;
	const size_t length = packet->length - EspNowLink::headerSize;
	if (linkHeader.session == EspNowLink::noSession)
	{
		/* A probe close by also answers the broadcasts on the channels next to its own: it says where it is */
		rememberProbe(packet->mac, linkHeader.channel ? linkHeader.channel : packet->channel, frame, length);
		if (firstAnswer(packet->mac) && !writeHost(frame, length))
			hostDrops++;
		releasePacket(packet);
		return;
	}
	/* Another session's, or a resend whose first copy came through */
	if (!fromSession || linkHeader.seq == rxSeq)
	{
		releasePacket(packet);
		return;
	}

	/* In order behind those that wait; it holds its pool place until the USB buffer takes it */
	rxSeq = linkHeader.seq;
	hostQueue[(hostQueueHead + hostQueueCount) % poolSize] = packet;
	hostQueueCount++;
	flushHost();
}

void Dongle::releasePacket(Packet* const packet)
{
	xQueueSend(freeQueue, &packet, 0);
}

bool Dongle::firstAnswer(const uint8_t* const mac)
{
	for (size_t i = 0; i < answeredCount; ++i)
		if (!memcmp(answered[i], mac, sizeof(answered[i])))
			return false;
	if (answeredCount < maxProbes)
		memcpy(answered[answeredCount++], mac, sizeof(answered[0]));
	return true;
}

void Dongle::rememberProbe(const uint8_t* const mac, const uint8_t channel, const uint8_t* const frame, const size_t length)
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
	slot->channel = channel;
	slot->used = true;
}

void Dongle::select(const Probe& probe)
{
	/* The CLOSE to the probe before goes on its channel, the sweep's broadcasts queued already on theirs */
	deselect(true);
	sweepChannel = 0;
	memcpy(probeMac, probe.mac, sizeof(probeMac));
	probeChannel = probe.channel;
	probeRssi = 0;

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
	/* Without it ESP-NOW sends at 1 Mb/s */
	ladder.reset();
	if (result == ESP_OK)
		result = EspNowLink::setPeerRate(probeMac, ladder.getRung());
	if (result != ESP_OK)
		ESP_LOGW(tag, "peer " MACSTR ": %s", MAC2STR(probeMac), esp_err_to_name(result));

	do
		session = static_cast<uint16_t>(esp_random());
	while (session == EspNowLink::noSession);
	txSeq = 1;
	lastDataSeq = 0;
	rxSeq = 0;
	advertisedLimit = 0;
	quietReported = false;
	lastSendUs = esp_timer_get_time();
	lastAckUs = lastSendUs;
	selected = true;
	ESP_LOGI(tag, "session %u with %s, " MACSTR " on channel %u", session, probe.id, MAC2STR(probeMac), probeChannel);
}

void Dongle::deselect(const bool tellProbe)
{
	if (!selected)
		return;
	/* What still waits for the probe belongs to the session */
	dropPacketsTo(probeMac);
	if (tellProbe)
		sendToProbe(EspNowLink::Kind::CLOSE, nullptr, 0);
	selected = false;
}

uint16_t Dongle::limit() const
{
	const UBaseType_t free = uxQueueMessagesWaiting(freeQueue);
	return static_cast<uint16_t>(rxSeq + (free > poolReserve ? free - poolReserve : 0));
}

bool Dongle::sendToProbe(const EspNowLink::Kind kind, const uint8_t* const frame, const size_t length)
{
	/* A PING tells the last DATA, so a probe that does not know the session tells it from one not started yet */
	uint16_t seq = 0;
	if (kind == EspNowLink::Kind::DATA)
		seq = txSeq;
	else if (kind == EspNowLink::Kind::PING)
		seq = lastDataSeq;
	const EspNowLink::Header header = {kind, session, seq, limit(), 0};
	if (!queuePacket(probeMac, probeChannel, header, frame, length, kind == EspNowLink::Kind::CLOSE))
		return false;
	if (kind == EspNowLink::Kind::DATA)
	{
		lastDataSeq = txSeq;
		txSeq = static_cast<uint16_t>(txSeq + 1);
		if (txSeq == 0)
			txSeq = 1;
	}
	advertisedLimit = header.limit;
	lastSendUs = esp_timer_get_time();
	return true;
}

bool Dongle::queuePacket(const uint8_t* const mac, const uint8_t channel, EspNowLink::Header header, const uint8_t* const frame, const size_t length, const bool once)
{
	if (txCount == txQueueSize)
		return false;
	Outgoing& packet = txQueue[(txHead + txCount) % txQueueSize];
	memcpy(packet.mac, mac, sizeof(packet.mac));
	packet.channel = channel;
	packet.once = once;
	header.channel = channel;
	EspNowLink::encodeHeader(packet.data, header);
	if (length)
		memcpy(packet.data + EspNowLink::headerSize, frame, length);
	packet.length = static_cast<uint16_t>(EspNowLink::headerSize + length);
	txCount++;
	serviceTx();
	return true;
}

void Dongle::serviceTx()
{
	while (txCount)
	{
		const TxState state = txState;
		if (state == TxState::inFlight)
			return;
		const Outgoing& packet = txQueue[txHead];
		if (txFrontSent)
		{
			if (state == TxState::quiet)
			{
				if (esp_timer_get_time() < quietUntilUs)
					return;
			}
			else
			{
				/* A resend goes at the rung the failure leaves, and at the lowest one after a pause */
				if (state != TxState::noBuffer)
					learnRate(packet.mac, state == TxState::idle);
				if (state == TxState::idle && !memcmp(packet.mac, probeMac, sizeof(probeMac)))
					quietReported = false;
				if (state == TxState::idle || txResends == EspNowLink::maxResends || (packet.once && state == TxState::failed))
				{
					/* Through, or given up */
					if (state != TxState::idle)
						txLost++;
					txState = TxState::idle;
					txHead = (txHead + 1) % txQueueSize;
					txCount--;
					txFrontSent = false;
					continue;
				}
				if (state == TxState::failed && startQuiet(packet))
					return;
			}
			txResends++;
		}
		else
			txResends = 0;

		/* Only now, with nothing in the air */
		setChannel(packet.channel);
		txFrontSent = true;
		txState = TxState::inFlight;
		txStartUs = esp_timer_get_time();
		const esp_err_t result = esp_now_send(packet.mac, packet.data, packet.length);
		if (result == ESP_OK)
			return;
		/* Out of buffers: it counts as a try, the next round goes again; refused: it is given up */
		txState = TxState::noBuffer;
		if (result != ESP_ERR_ESPNOW_NO_MEM)
		{
			ESP_LOGW(tag, "esp_now_send(): %s", esp_err_to_name(result));
			txResends = EspNowLink::maxResends;
		}
		return;
	}
}

bool Dongle::startQuiet(const Outgoing& packet)
{
	if (!selected || memcmp(packet.mac, probeMac, sizeof(probeMac)) || !ladder.atBottom())
		return false;
	const int64_t tryUs = txEndUs.load() - txStartUs;
	const int64_t pauseUs = EspNowLink::quietUs(tryUs);
	quietUntilUs = esp_timer_get_time() + pauseUs;
	txState = TxState::quiet;
	if (!quietReported)
	{
		ESP_LOGW(tag, "no acknowledgement from the probe at %s (the try took %lld ms): one try every %lld ms until it is heard", EspNowLink::rates[ladder.getRung()].name, static_cast<long long>(tryUs / 1000), static_cast<long long>((tryUs + pauseUs) / 1000));
		quietReported = true;
	}
	return true;
}

void Dongle::dropPacketsTo(const uint8_t* const mac)
{
	size_t kept = 0;
	for (size_t i = 0; i < txCount; ++i)
	{
		const size_t from = (txHead + i) % txQueueSize;
		const bool inAir = i == 0 && txFrontSent && txState == TxState::inFlight;
		if (!inAir && !memcmp(txQueue[from].mac, mac, sizeof(txQueue[from].mac)))
		{
			/* A front packet waiting to go again is given up */
			if (i == 0 && txFrontSent)
			{
				txFrontSent = false;
				txState = TxState::idle;
			}
			continue;
		}
		/* The radio has it: it is not sent again */
		if (inAir && !memcmp(txQueue[from].mac, mac, sizeof(txQueue[from].mac)))
			txResends = EspNowLink::maxResends;
		const size_t to = (txHead + kept) % txQueueSize;
		if (to != from)
			txQueue[to] = txQueue[from];
		kept++;
	}
	txCount = kept;
}

void Dongle::setChannel(const uint8_t wanted)
{
	if (wanted == channel)
		return;
	const esp_err_t result = esp_wifi_set_channel(wanted, WIFI_SECOND_CHAN_NONE);
	if (result == ESP_OK)
		channel = wanted;
	else
		ESP_LOGW(tag, "channel %u: %s", wanted, esp_err_to_name(result));
}

void Dongle::learnRate(const uint8_t* const mac, const bool acknowledged)
{
	/* Broadcasts get no acknowledgement, and keep 1 Mb/s for range */
	if (!selected || memcmp(mac, probeMac, sizeof(probeMac)) || !ladder.update(acknowledged, esp_timer_get_time()))
		return;
	const uint8_t rung = ladder.getRung();
	const esp_err_t result = EspNowLink::setPeerRate(probeMac, rung);
	if (result == ESP_OK)
		ESP_LOGI(tag, "sending to the probe at %s", EspNowLink::rates[rung].name);
	else
		ESP_LOGW(tag, "rate %s: %s", EspNowLink::rates[rung].name, esp_err_to_name(result));
}

void Dongle::startSweep(const uint8_t* const frame, const size_t length)
{
	memcpy(sweepFrame, frame, length);
	sweepLength = length;
	sweepChannel = firstChannel;
	sweepCopy = 0;
	sweepNextUs = 0;
	serviceSweep();
}

void Dongle::serviceSweep()
{
	if (!sweepChannel)
		return;
	const int64_t now = esp_timer_get_time();
	if (now < sweepNextUs)
		return;
	if (sweepCopy == sweepCopies)
	{
		/* The answers had their time: the next channel, or the sweep is over */
		sweepCopy = 0;
		if (sweepChannel == lastChannel)
		{
			sweepChannel = 0;
			return;
		}
		sweepChannel++;
	}
	/* A full queue skips a copy */
	queuePacket(broadcastMac, sweepChannel, {EspNowLink::Kind::DATA, EspNowLink::noSession, 0, 0, 0}, sweepFrame, sweepLength);
	sweepCopy++;
	sweepNextUs = now + sweepDwellMs * 1000LL / sweepCopies;
}

void Dongle::serviceSession()
{
	if (!selected)
		return;
	const int64_t now = esp_timer_get_time();
	if (now - lastAckUs > EspNowLink::sessionTimeoutMs * 1000LL)
	{
		ESP_LOGW(tag, "the probe stopped answering: session %u ends", session);
		deselect(false);
		alert = true;
		return;
	}

	/* A raised limit goes out early, before the probe runs out of it; a PING keeps the session alive */
	const uint16_t fresh = limit();
	const bool raised = static_cast<int16_t>(static_cast<uint16_t>(fresh - advertisedLimit)) >= static_cast<int16_t>(creditStep);
	const bool starving = fresh != advertisedLimit && !EspNowLink::allows(advertisedLimit, static_cast<uint16_t>(rxSeq + 1));
	if (raised || starving || now - lastSendUs >= EspNowLink::pingIntervalMs * 1000LL)
		sendToProbe(EspNowLink::Kind::PING, nullptr, 0);
}
