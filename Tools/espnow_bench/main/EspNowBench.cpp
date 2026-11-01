/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: MIT
 *
 * ESP-NOW bench: round trip, throughput and losses between two ESP32-S3 boards that run this firmware, at the PHY
 * rates below, with and without encryption. Both boards answer; the BOOT button makes a board the initiator, which
 * finds the other one, runs every test and prints a summary. It answers what the ESP-NOW transport depends on before
 * it is built: how fast it is, how much gets lost and where, whether encryption works with v2 frames, and what a
 * station link on the same radio costs.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <iterator>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

namespace
{
constexpr const char* tag = "espnow_bench";

constexpr size_t maxFrame = ESP_NOW_MAX_DATA_LEN_V2;
constexpr size_t smallFrame = 32;
constexpr uint32_t rxBufferCount = 32;
constexpr uint32_t maxPings = 200;
constexpr int64_t rttTestUs = 1500000;		 /**< A round trip test ends after this or maxPings. */
constexpr int64_t pingTimeoutUs = 100000;	 /**< Long enough for 1470 bytes both ways at LR 500 kb/s. */
constexpr int64_t replyTimeoutUs = 100000;	 /**< Before SETUP, STATS_REQ or STREAM_REQ is sent again. */
constexpr int attempts = 5;
constexpr int64_t findTimeoutUs = 5000000;	 /**< A full hop round of the responder takes 2 s. */
constexpr int64_t helloIntervalUs = 20000;
constexpr int64_t idleUs = 5000000;			 /**< The responder hops again this long after the initiator's last frame. */
constexpr int64_t hopDwellUs = 150000;
constexpr uint8_t maxChannel = 13;
constexpr uint32_t maxStreamMs = 10000;
constexpr uint32_t joinTimeoutMs = 15000;
constexpr uint32_t joinRetryDelayMs = 500;
constexpr uint32_t taskStackSize = 8192;
constexpr uint32_t taskPriority = 5;
constexpr int taskCore = 0; /**< With Wi-Fi, like the probe's net task. */
constexpr bool joinsNetwork = sizeof(CONFIG_ESPNOW_BENCH_WIFI_SSID) > 1;

constexpr uint8_t broadcastMac[ESP_NOW_ETH_ALEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
/* The same keys on every board: the transport derives one per pairing */
constexpr uint8_t pmk[ESP_NOW_KEY_LEN] = {'e', 's', 'p', 'n', 'o', 'w', '-', 'b', 'e', 'n', 'c', 'h', '-', 'p', 'm', 'k'};
constexpr uint8_t lmk[ESP_NOW_KEY_LEN] = {'e', 's', 'p', 'n', 'o', 'w', '-', 'b', 'e', 'n', 'c', 'h', '-', 'l', 'm', 'k'};

constexpr EventBits_t gotIpBit = BIT0;
constexpr EventBits_t disconnectedBit = BIT1;
constexpr uint8_t legacyProtocols = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;

/* Control frames up to SETUP_ACK go out as broadcasts, which are never encrypted, so a key change cannot cut them off */
enum class Type : uint8_t
{
	HELLO = 1,	/* initiator: who is there */
	HELLO_ACK,	/* responder */
	SETUP,		/* initiator: u8 rate index, u8 encrypt */
	SETUP_ACK,	/* responder, with the rate and key applied: u8 ok */
	PING,		/* unicast, initiator: padding up to the frame size */
	PONG,		/* unicast, responder: the PING sent back */
	STREAM,		/* unicast: padding up to the frame size */
	STATS_REQ,	/* unicast, initiator: what arrived of the test's STREAM frames */
	STATS,		/* unicast, responder: RxReport */
	STREAM_REQ, /* unicast, initiator: u32 duration ms, u16 frame size; the responder streams back */
	STREAM_END, /* unicast, responder, three times: TxReport */
};

struct __attribute__((packed)) Header
{
	Type type;
	uint8_t test;	  /* A new one for every test, so late frames of the previous one are told apart */
	uint16_t session; /* The initiator's, so other benches nearby are ignored */
	uint32_t seq;
};

/* The receiving side of a stream */
struct __attribute__((packed)) RxReport
{
	uint32_t received;
	uint32_t duplicates; /* seq not above one received before */
	uint32_t gaps;		 /* seq skipped */
	uint32_t queueDrops; /* No free buffer in the receive callback: the application did not keep up */
	uint32_t bytes;
	uint32_t spanUs; /* First to last frame */
	int8_t rssi;	 /* Average, dBm */
};

/* The sending side of a stream */
struct __attribute__((packed)) TxReport
{
	uint32_t sent;	   /* Taken by esp_now_send() */
	uint32_t macOk;	   /* The peer's MAC acknowledged */
	uint32_t macFail;  /* No acknowledgement after the MAC's retries */
	uint32_t noMem;	   /* esp_now_send() out of buffers, tried again */
	uint32_t errors;   /* esp_now_send() refused the frame */
};

struct Rate
{
	const char* name;
	wifi_phy_mode_t mode;
	wifi_phy_rate_t rate;
};

/*
 * From 802.11b to the fastest, then Espressif's long range mode. 1M, the ESP-NOW default, is left out: at it the senders
 * never heard an ACK, so every frame arrived but took all the MAC's retries, and the retries spilled into the next test.
 */
constexpr Rate rates[] = {
	{"11M", WIFI_PHY_MODE_11B, WIFI_PHY_RATE_11M_S},
	{"24M", WIFI_PHY_MODE_11G, WIFI_PHY_RATE_24M},
	{"54M", WIFI_PHY_MODE_11G, WIFI_PHY_RATE_54M},
	{"MCS3", WIFI_PHY_MODE_HT20, WIFI_PHY_RATE_MCS3_LGI},
	{"MCS5", WIFI_PHY_MODE_HT20, WIFI_PHY_RATE_MCS5_LGI},
	{"MCS7S", WIFI_PHY_MODE_HT20, WIFI_PHY_RATE_MCS7_SGI},
	{"LR500", WIFI_PHY_MODE_LR, WIFI_PHY_RATE_LORA_500K},
};
constexpr size_t rateCount = std::size(rates);

struct Rtt
{
	uint32_t pings;
	uint32_t lost;
	float avgMs;
	float p95Ms;
	float maxMs;
};

struct Throughput
{
	bool done; /* The other side reported */
	TxReport tx;
	RxReport rx;
};

struct Result
{
	bool ok; /* Both boards took the rate and the key */
	Rtt small;
	Rtt large;
	Throughput forward; /* The initiator sends */
	Throughput reverse; /* The responder sends */
};

float kbPerSecond(const RxReport& rx)
{
	return rx.spanUs ? rx.bytes * 1000.0f / rx.spanUs : 0.0f;
}

/* Frames handed to ESP-NOW that the application on the other side never got */
float lossPercent(const Throughput& throughput)
{
	if (!throughput.tx.sent)
		return 100.0f;
	const uint32_t unique = throughput.rx.received - throughput.rx.duplicates;
	return 100.0f * (1.0f - static_cast<float>(unique) / throughput.tx.sent);
}

/* Average and 95th percentile in ms, and the pings that got no answer */
void formatRtt(char* const out, const size_t size, const Rtt& rtt)
{
	if (rtt.lost == rtt.pings)
		snprintf(out, size, "no answer to %" PRIu32, rtt.pings);
	else if (rtt.lost)
		snprintf(out, size, "%6.2f / %6.2f, %" PRIu32 " lost", rtt.avgMs, rtt.p95Ms, rtt.lost);
	else
		snprintf(out, size, "%6.2f / %6.2f", rtt.avgMs, rtt.p95Ms);
}

const char* keyName(const bool encrypt)
{
	return encrypt ? "lmk" : "open";
}

/**
 * @brief Both roles of the bench. The responder answers from the main loop; the initiator runs the tests in the same
 * task, so only one role is active at a time.
 */
class Bench
{
   public:
	void start();

   private:
	struct RxBuffer
	{
		int64_t time; /* When the receive callback ran */
		uint8_t src[ESP_NOW_ETH_ALEN];
		int8_t rssi;
		uint16_t length;
		uint8_t data[maxFrame];
	};

	/* The STREAM frames of the current test */
	struct Count
	{
		bool active;
		uint8_t test;
		uint32_t received;
		uint32_t duplicates;
		uint32_t gaps;
		uint32_t bytes;
		uint32_t expected; /* Next seq */
		uint32_t dropsAtStart;
		int64_t firstUs;
		int64_t lastUs;
		int32_t rssiSum;
	};

	static void taskEntry(void* arg);
	static void onReceive(const esp_now_recv_info_t* info, const uint8_t* data, int length);
	static void onSent(const esp_now_send_info_t* info, esp_now_send_status_t status);
	static void onEvent(void* arg, esp_event_base_t base, int32_t id, void* data);

	[[noreturn]] void run();
	void startWifi();
	void startButton();

	/**
	 * @return True once the button was pressed and released.
	 */
	bool buttonPressed();

	/**
	 * @brief Moves the responder to the next channel while no initiator is around.
	 */
	void hop(int64_t now);

	void respond(RxBuffer& buffer);
	void startSession(const RxBuffer& buffer);
	void streamBack(uint8_t test, const uint8_t* payload, size_t length);

	void runInitiator();
	bool joinNetwork();

	/**
	 * @brief Disconnects the station and offers LR again.
	 */
	void leaveNetwork();
	bool findResponder();
	Result runConfig(size_t rate, bool encrypt);
	bool setUp(size_t rate, bool encrypt);
	Rtt measureRtt(size_t size);
	Throughput measureForward();
	Throughput measureReverse();
	void logResult(size_t rate, bool encrypt, const Result& result) const;
	void printSummary() const;

	/**
	 * @brief Sets the key and the rate the peer is sent to with.
	 */
	bool apply(size_t rate, bool encrypt);
	void addPeer(const uint8_t* mac);

	/**
	 * @brief Writes a header to txFrame.
	 * @return Where the payload goes.
	 */
	uint8_t* prepare(Type type, uint8_t test, uint32_t seq);
	bool send(const uint8_t* mac, size_t length) { return sendFrame(mac, txFrame, length); }

	/**
	 * @brief Sends once a window slot is free; the send callback frees it again.
	 */
	bool sendFrame(const uint8_t* mac, const uint8_t* data, size_t length);

	/**
	 * @brief Waits until every frame sent has had its send callback.
	 */
	void waitSent();

	/**
	 * @brief Sends STREAM frames of size bytes to the peer for durationMs.
	 */
	TxReport stream(uint8_t test, uint32_t durationMs, size_t size);

	void resetCount(uint8_t test);
	void count(const RxBuffer& buffer);
	RxReport report(uint8_t test) const;

	/**
	 * @return The next frame received before the deadline, or nullptr. Give it back with release().
	 */
	RxBuffer* receive(int64_t deadline);

	/**
	 * @return The next frame of the session with this type and test, or nullptr; others are dropped.
	 */
	RxBuffer* waitFor(Type type, uint8_t test, int64_t deadline);
	void release(RxBuffer* buffer) { xQueueSend(freeQueue, &buffer, 0); }

	static Header& headerOf(RxBuffer& buffer) { return *reinterpret_cast<Header*>(buffer.data); }
	static const Header& headerOf(const RxBuffer& buffer) { return *reinterpret_cast<const Header*>(buffer.data); }
	bool isFromPeer(const RxBuffer& buffer) const { return !memcmp(buffer.src, peer, sizeof(peer)); }
	uint8_t nextTest() { return ++testId; }

	static Bench* instance;

	RxBuffer pool[rxBufferCount];
	QueueHandle_t freeQueue = nullptr; /* RxBuffer pointers */
	QueueHandle_t rxQueue = nullptr;
	SemaphoreHandle_t txSlots = nullptr;
	EventGroupHandle_t events = nullptr;
	std::atomic<uint32_t> queueDrops{0};
	std::atomic<uint32_t> macOk{0};
	std::atomic<uint32_t> macFail{0};
	std::atomic<bool> joined{false};
	uint32_t sent = 0;
	uint32_t noMem = 0;
	uint32_t sendErrors = 0;
	bool errorLogged = false;

	uint8_t txFrame[maxFrame];
	uint8_t peer[ESP_NOW_ETH_ALEN] = {};
	bool hasPeer = false;
	uint16_t session = 0;
	uint8_t testId = 0;
	uint8_t channel = CONFIG_ESPNOW_BENCH_CHANNEL;
	int64_t lastPeerUs = 0;
	int64_t nextHopUs = 0;
	int lastStreamTest = -1; /* The responder streams once per STREAM_REQ, however often it is sent */
	Count counted = {};
	Result results[rateCount][2] = {};
};

Bench* Bench::instance = nullptr;

void Bench::start()
{
	instance = this;
	freeQueue = xQueueCreate(rxBufferCount, sizeof(RxBuffer*));
	rxQueue = xQueueCreate(rxBufferCount, sizeof(RxBuffer*));
	for (RxBuffer& buffer : pool)
		release(&buffer);
	txSlots = xSemaphoreCreateCounting(CONFIG_ESPNOW_BENCH_WINDOW, CONFIG_ESPNOW_BENCH_WINDOW);
	events = xEventGroupCreate();

	startWifi();
	startButton();
	xTaskCreatePinnedToCore(taskEntry, "bench", taskStackSize, this, taskPriority, nullptr, taskCore);
}

void Bench::taskEntry(void* const arg)
{
	static_cast<Bench*>(arg)->run();
}

void Bench::onReceive(const esp_now_recv_info_t* const info, const uint8_t* const data, const int length)
{
	/* Wi-Fi task: copy and hand over, like the transport will */
	Bench& self = *instance;
	if (length < static_cast<int>(sizeof(Header)) || length > static_cast<int>(maxFrame))
		return;
	RxBuffer* buffer = nullptr;
	if (xQueueReceive(self.freeQueue, &buffer, 0) != pdTRUE)
	{
		self.queueDrops.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	buffer->time = esp_timer_get_time();
	memcpy(buffer->src, info->src_addr, ESP_NOW_ETH_ALEN);
	buffer->rssi = static_cast<int8_t>(info->rx_ctrl->rssi);
	buffer->length = static_cast<uint16_t>(length);
	memcpy(buffer->data, data, length);
	xQueueSend(self.rxQueue, &buffer, 0);
}

void Bench::onSent(const esp_now_send_info_t* const info, const esp_now_send_status_t status)
{
	Bench& self = *instance;
	(status == ESP_NOW_SEND_SUCCESS ? self.macOk : self.macFail).fetch_add(1, std::memory_order_relaxed);
	xSemaphoreGive(self.txSlots);
}

void Bench::onEvent(void* const arg, const esp_event_base_t base, const int32_t id, void* const data)
{
	Bench& self = *static_cast<Bench*>(arg);
	if (base == IP_EVENT)
	{
		self.joined = true;
		xEventGroupSetBits(self.events, gotIpBit);
		return;
	}
	self.joined = false;
	xEventGroupSetBits(self.events, disconnectedBit);
	ESP_LOGW(tag, "station disconnected, reason %u", static_cast<const wifi_event_sta_disconnected_t*>(data)->reason);
}

void Bench::run()
{
	uint8_t mac[ESP_NOW_ETH_ALEN];
	esp_read_mac(mac, ESP_MAC_WIFI_STA);
	uint32_t version = 0;
	esp_now_get_version(&version);
	ESP_LOGI(tag, "MAC " MACSTR ", ESP-NOW v%" PRIu32 ". Waiting for an initiator; press BOOT to run the bench from this board", MAC2STR(mac), version);

	while (true)
	{
		RxBuffer* const buffer = receive(esp_timer_get_time() + 10000);
		if (buffer)
		{
			respond(*buffer);
			release(buffer);
		}
		if (buttonPressed())
			runInitiator();
		hop(esp_timer_get_time());
	}
}

void Bench::startWifi()
{
	esp_err_t result = nvs_flash_init();
	if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND)
	{
		ESP_ERROR_CHECK(nvs_flash_erase());
		result = nvs_flash_init();
	}
	ESP_ERROR_CHECK(result);

	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_create_default_wifi_sta();
	const wifi_init_config_t initConfig = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&initConfig));
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, onEvent, this));
	ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, onEvent, this));
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_start());
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
	/* LR next to b/g/n: the station still joins ordinary access points */
	ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, legacyProtocols | WIFI_PROTOCOL_LR));
	ESP_ERROR_CHECK(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE));

	ESP_ERROR_CHECK(esp_now_init());
	ESP_ERROR_CHECK(esp_now_register_recv_cb(onReceive));
	ESP_ERROR_CHECK(esp_now_register_send_cb(onSent));
	ESP_ERROR_CHECK(esp_now_set_pmk(pmk));
	addPeer(broadcastMac);
}

void Bench::startButton()
{
	gpio_config_t config = {};
	config.pin_bit_mask = 1ULL << CONFIG_ESPNOW_BENCH_BUTTON_GPIO;
	config.mode = GPIO_MODE_INPUT;
	config.pull_up_en = GPIO_PULLUP_ENABLE;
	ESP_ERROR_CHECK(gpio_config(&config));
}

bool Bench::buttonPressed()
{
	const auto button = static_cast<gpio_num_t>(CONFIG_ESPNOW_BENCH_BUTTON_GPIO);
	if (gpio_get_level(button))
		return false;
	/* One press, one run */
	while (!gpio_get_level(button))
		vTaskDelay(pdMS_TO_TICKS(10));
	return true;
}

void Bench::hop(const int64_t now)
{
	if (joined || now - lastPeerUs < idleUs || now < nextHopUs)
		return;
	channel = static_cast<uint8_t>(channel % maxChannel + 1);
	esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
	nextHopUs = now + hopDwellUs;
}

void Bench::respond(RxBuffer& buffer)
{
	Header& header = headerOf(buffer);
	if (header.type == Type::HELLO)
	{
		startSession(buffer);
		prepare(Type::HELLO_ACK, header.test, 0);
		send(broadcastMac, sizeof(Header));
		return;
	}
	if (!hasPeer || header.session != session || !isFromPeer(buffer))
		return;

	lastPeerUs = buffer.time;
	const uint8_t* const payload = buffer.data + sizeof(Header);
	const size_t payloadLength = buffer.length - sizeof(Header);
	switch (header.type)
	{
		case Type::SETUP:
		{
			if (payloadLength < 2)
				break;
			/* Applied before the answer, so the initiator's first unicast frame finds the key in place */
			const bool ok = payload[0] < rateCount && apply(payload[0], payload[1]);
			prepare(Type::SETUP_ACK, header.test, 0)[0] = ok;
			send(broadcastMac, sizeof(Header) + 1);
			break;
		}
		case Type::PING:
			header.type = Type::PONG;
			sendFrame(peer, buffer.data, buffer.length);
			break;
		case Type::STREAM:
			count(buffer);
			break;
		case Type::STATS_REQ:
		{
			const RxReport rx = report(header.test);
			memcpy(prepare(Type::STATS, header.test, 0), &rx, sizeof(rx));
			send(peer, sizeof(Header) + sizeof(rx));
			break;
		}
		case Type::STREAM_REQ:
			if (header.test != lastStreamTest)
				streamBack(header.test, payload, payloadLength);
			break;
		default:
			break;
	}
}

void Bench::startSession(const RxBuffer& buffer)
{
	const Header& header = headerOf(buffer);
	lastPeerUs = buffer.time;
	if (hasPeer && header.session == session && isFromPeer(buffer))
		return;

	session = header.session;
	memcpy(peer, buffer.src, sizeof(peer));
	addPeer(peer);
	hasPeer = true;
	lastStreamTest = -1;
	counted.active = false;
	ESP_LOGI(tag, "initiator " MACSTR " found on channel %u, RSSI %d", MAC2STR(peer), channel, buffer.rssi);
}

void Bench::streamBack(const uint8_t test, const uint8_t* const payload, const size_t length)
{
	if (length < 6)
		return;
	uint32_t durationMs = 0;
	uint16_t size = 0;
	memcpy(&durationMs, payload, sizeof(durationMs));
	memcpy(&size, payload + 4, sizeof(size));
	lastStreamTest = test;

	const TxReport tx = stream(test, std::min(durationMs, maxStreamMs), std::clamp<size_t>(size, sizeof(Header), maxFrame));
	/* Three times rather than a handshake: each copy has the MAC's retries behind it */
	for (int i = 0; i < 3; ++i)
	{
		memcpy(prepare(Type::STREAM_END, test, 0), &tx, sizeof(tx));
		send(peer, sizeof(Header) + sizeof(tx));
	}
}

void Bench::runInitiator()
{
	ESP_LOGI(tag, "running the bench from this board");
	session = static_cast<uint16_t>(esp_random());
	testId = static_cast<uint8_t>(esp_random());
	hasPeer = false;

	if (joinsNetwork)
	{
		if (!joinNetwork())
		{
			leaveNetwork();
			return;
		}
		wifi_second_chan_t second;
		esp_wifi_get_channel(&channel, &second);
	}
	else
	{
		channel = CONFIG_ESPNOW_BENCH_CHANNEL;
		esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
	}

	if (findResponder())
	{
		for (size_t rate = 0; rate < rateCount; ++rate)
			for (int encrypt = 0; encrypt < 2; ++encrypt)
				results[rate][encrypt] = runConfig(rate, encrypt);
		printSummary();
	}

	if (joinsNetwork)
		leaveNetwork();
	/* Back to answering */
	hasPeer = false;
	lastPeerUs = esp_timer_get_time();
}

bool Bench::joinNetwork()
{
	wifi_config_t config = {};
	strlcpy(reinterpret_cast<char*>(config.sta.ssid), CONFIG_ESPNOW_BENCH_WIFI_SSID, sizeof(config.sta.ssid));
	strlcpy(reinterpret_cast<char*>(config.sta.password), CONFIG_ESPNOW_BENCH_WIFI_PASSWORD, sizeof(config.sta.password));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
	ESP_LOGI(tag, "joining %s", CONFIG_ESPNOW_BENCH_WIFI_SSID);

	/* An access point may leave an authentication unanswered: try again until the timeout, like the probe does */
	const int64_t deadline = esp_timer_get_time() + joinTimeoutMs * 1000LL;
	bool withLr = true;
	while (true)
	{
		const int64_t remainingUs = deadline - esp_timer_get_time();
		if (remainingUs <= 0)
		{
			ESP_LOGE(tag, "could not join %s", CONFIG_ESPNOW_BENCH_WIFI_SSID);
			return false;
		}
		xEventGroupClearBits(events, gotIpBit | disconnectedBit);
		esp_wifi_connect();
		const EventBits_t bits = xEventGroupWaitBits(events, gotIpBit | disconnectedBit, pdFALSE, pdFALSE, pdMS_TO_TICKS(remainingUs / 1000));
		if (bits & gotIpBit)
			break;
		if (bits & disconnectedBit)
		{
			/* Whether offering LR keeps the access point from answering: the second half of the time goes without it */
			if (withLr && remainingUs < joinTimeoutMs * 500LL)
			{
				withLr = false;
				ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_protocol(WIFI_IF_STA, legacyProtocols));
				ESP_LOGW(tag, "trying without LR");
			}
			vTaskDelay(pdMS_TO_TICKS(joinRetryDelayMs));
		}
	}

	wifi_ap_record_t ap = {};
	esp_wifi_sta_get_ap_info(&ap);
	ESP_LOGI(tag, "joined %s on channel %u, RSSI %d%s", CONFIG_ESPNOW_BENCH_WIFI_SSID, ap.primary, ap.rssi, withLr ? "" : ", without LR");
	return true;
}

void Bench::leaveNetwork()
{
	xEventGroupClearBits(events, disconnectedBit);
	esp_wifi_disconnect();
	xEventGroupWaitBits(events, disconnectedBit, pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));
	/* LR back for the next session, in case the join went without it */
	ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_protocol(WIFI_IF_STA, legacyProtocols | WIFI_PROTOCOL_LR));
}

bool Bench::findResponder()
{
	const uint8_t test = nextTest();
	const int64_t deadline = esp_timer_get_time() + findTimeoutUs;
	while (esp_timer_get_time() < deadline)
	{
		prepare(Type::HELLO, test, 0);
		send(broadcastMac, sizeof(Header));
		if (RxBuffer* const buffer = waitFor(Type::HELLO_ACK, test, esp_timer_get_time() + helloIntervalUs))
		{
			memcpy(peer, buffer->src, sizeof(peer));
			ESP_LOGI(tag, "responder " MACSTR " on channel %u, RSSI %d", MAC2STR(peer), channel, buffer->rssi);
			release(buffer);
			addPeer(peer);
			hasPeer = true;
			return true;
		}
	}
	ESP_LOGE(tag, "no responder answered on channel %u", channel);
	return false;
}

Result Bench::runConfig(const size_t rate, const bool encrypt)
{
	Result result = {};
	if (!setUp(rate, encrypt))
	{
		ESP_LOGW(tag, "%s %s: the boards did not agree on the setup", rates[rate].name, keyName(encrypt));
		return result;
	}
	result.ok = true;
	result.small = measureRtt(smallFrame);
	result.large = measureRtt(maxFrame);
	result.forward = measureForward();
	result.reverse = measureReverse();
	logResult(rate, encrypt, result);
	return result;
}

bool Bench::setUp(const size_t rate, const bool encrypt)
{
	const uint8_t test = nextTest();
	for (int attempt = 0; attempt < 2 * attempts; ++attempt)
	{
		uint8_t* const payload = prepare(Type::SETUP, test, 0);
		payload[0] = static_cast<uint8_t>(rate);
		payload[1] = encrypt;
		send(broadcastMac, sizeof(Header) + 2);
		if (RxBuffer* const buffer = waitFor(Type::SETUP_ACK, test, esp_timer_get_time() + replyTimeoutUs))
		{
			const bool remoteOk = buffer->length > sizeof(Header) && buffer->data[sizeof(Header)];
			release(buffer);
			const bool localOk = apply(rate, encrypt);
			return remoteOk && localOk;
		}
	}
	return false;
}

Rtt Bench::measureRtt(const size_t size)
{
	const uint8_t test = nextTest();
	float samples[maxPings];
	uint32_t received = 0;
	Rtt rtt = {};
	memset(txFrame + sizeof(Header), 0x5a, size - sizeof(Header));

	const int64_t end = esp_timer_get_time() + rttTestUs;
	for (uint32_t seq = 0; seq < maxPings && esp_timer_get_time() < end; ++seq)
	{
		prepare(Type::PING, test, seq);
		rtt.pings++;
		const int64_t sentUs = esp_timer_get_time();
		if (!send(peer, size))
		{
			rtt.lost++;
			continue;
		}
		/* PONGs of pings that timed out come late: skip them */
		RxBuffer* buffer;
		while ((buffer = waitFor(Type::PONG, test, sentUs + pingTimeoutUs)) && headerOf(*buffer).seq != seq)
			release(buffer);
		if (!buffer)
		{
			rtt.lost++;
			continue;
		}
		samples[received++] = (buffer->time - sentUs) / 1000.0f;
		release(buffer);
	}

	if (received)
	{
		std::sort(samples, samples + received);
		float sum = 0.0f;
		for (uint32_t i = 0; i < received; ++i)
			sum += samples[i];
		rtt.avgMs = sum / received;
		rtt.p95Ms = samples[std::min(received - 1, received * 95 / 100)];
		rtt.maxMs = samples[received - 1];
	}
	return rtt;
}

Throughput Bench::measureForward()
{
	const uint8_t test = nextTest();
	Throughput throughput = {};
	throughput.tx = stream(test, CONFIG_ESPNOW_BENCH_STREAM_MS, maxFrame);

	/* The responder handles its frames in order, so STATS_REQ finds every STREAM frame counted */
	for (int attempt = 0; attempt < attempts && !throughput.done; ++attempt)
	{
		prepare(Type::STATS_REQ, test, 0);
		send(peer, sizeof(Header));
		if (RxBuffer* const buffer = waitFor(Type::STATS, test, esp_timer_get_time() + replyTimeoutUs))
		{
			if (buffer->length >= sizeof(Header) + sizeof(RxReport))
			{
				memcpy(&throughput.rx, buffer->data + sizeof(Header), sizeof(RxReport));
				throughput.done = true;
			}
			release(buffer);
		}
	}
	return throughput;
}

Throughput Bench::measureReverse()
{
	const uint8_t test = nextTest();
	Throughput throughput = {};
	resetCount(test);

	const int64_t deadline = esp_timer_get_time() + CONFIG_ESPNOW_BENCH_STREAM_MS * 1000LL + 2000000;
	int requests = 0;
	int64_t retryUs = 0;
	while (!throughput.done && esp_timer_get_time() < deadline)
	{
		/* STREAM_REQ again until the stream starts */
		if (!counted.received && esp_timer_get_time() >= retryUs)
		{
			if (requests++ == attempts)
				break;
			uint8_t* const payload = prepare(Type::STREAM_REQ, test, 0);
			const uint32_t durationMs = CONFIG_ESPNOW_BENCH_STREAM_MS;
			const uint16_t size = maxFrame;
			memcpy(payload, &durationMs, sizeof(durationMs));
			memcpy(payload + 4, &size, sizeof(size));
			send(peer, sizeof(Header) + 6);
			retryUs = esp_timer_get_time() + replyTimeoutUs;
		}

		RxBuffer* const buffer = receive(counted.received ? deadline : retryUs);
		if (!buffer)
			continue;
		const Header& header = headerOf(*buffer);
		if (header.session == session && header.test == test && isFromPeer(*buffer))
		{
			if (header.type == Type::STREAM)
				count(*buffer);
			else if (header.type == Type::STREAM_END && buffer->length >= sizeof(Header) + sizeof(TxReport))
			{
				memcpy(&throughput.tx, buffer->data + sizeof(Header), sizeof(TxReport));
				throughput.done = true;
			}
		}
		release(buffer);
	}
	throughput.rx = report(test);
	return throughput;
}

void Bench::logResult(const size_t rate, const bool encrypt, const Result& result) const
{
	const char* const name = rates[rate].name;
	const char* const key = keyName(encrypt);
	ESP_LOGI(tag, "%s %s: rtt %u B avg %.2f p95 %.2f max %.2f ms, lost %" PRIu32 "/%" PRIu32 "; rtt %u B avg %.2f p95 %.2f max %.2f ms, lost %" PRIu32 "/%" PRIu32, name, key, smallFrame, result.small.avgMs, result.small.p95Ms,
			 result.small.maxMs, result.small.lost, result.small.pings, maxFrame, result.large.avgMs, result.large.p95Ms, result.large.maxMs, result.large.lost, result.large.pings);

	const Throughput* const directions[] = {&result.forward, &result.reverse};
	const char* const directionNames[] = {"fwd", "rev"};
	for (int i = 0; i < 2; ++i)
	{
		const Throughput& t = *directions[i];
		ESP_LOGI(tag,
				 "%s %s %s: %.1f kB/s, loss %.1f%%; sent %" PRIu32 ", mac ok %" PRIu32 " fail %" PRIu32 ", no_mem %" PRIu32 ", errors %" PRIu32 "; received %" PRIu32 ", dup %" PRIu32 ", gaps %" PRIu32 ", queue drops %" PRIu32
				 ", rssi %d%s",
				 name, key, directionNames[i], kbPerSecond(t.rx), lossPercent(t), t.tx.sent, t.tx.macOk, t.tx.macFail, t.tx.noMem, t.tx.errors, t.rx.received, t.rx.duplicates, t.rx.gaps, t.rx.queueDrops, t.rx.rssi,
				 t.done ? "" : " (the other side did not report)");
	}
}

void Bench::printSummary() const
{
	printf("\nESP-NOW bench: channel %u%s, %d frame(s) in flight, %d ms streams of %u-byte frames, peer " MACSTR "\n", channel, joinsNetwork ? " (station joined)" : "", CONFIG_ESPNOW_BENCH_WINDOW, CONFIG_ESPNOW_BENCH_STREAM_MS, maxFrame,
		   MAC2STR(peer));
	printf("fwd: this board sends, rev: the other one; loss: frames sent that the other application never got\n\n");
	char smallTitle[32];
	char largeTitle[32];
	snprintf(smallTitle, sizeof(smallTitle), "rtt %u B avg/p95 ms", smallFrame);
	snprintf(largeTitle, sizeof(largeTitle), "rtt %u B avg/p95 ms", maxFrame);
	printf("rate   key  | %-26s | %-26s | fwd kB/s   loss | rev kB/s   loss\n", smallTitle, largeTitle);
	for (size_t rate = 0; rate < rateCount; ++rate)
	{
		for (int encrypt = 0; encrypt < 2; ++encrypt)
		{
			const Result& r = results[rate][encrypt];
			if (!r.ok)
			{
				printf("%-6s %-4s | setup failed\n", rates[rate].name, keyName(encrypt));
				continue;
			}
			char small[48];
			char large[48];
			formatRtt(small, sizeof(small), r.small);
			formatRtt(large, sizeof(large), r.large);
			printf("%-6s %-4s | %-26s | %-26s | %8.1f %5.1f%% | %8.1f %5.1f%%\n", rates[rate].name, keyName(encrypt), small, large, kbPerSecond(r.forward.rx), lossPercent(r.forward), kbPerSecond(r.reverse.rx),
				   lossPercent(r.reverse));
		}
	}
	printf("\n");
}

bool Bench::apply(const size_t rate, const bool encrypt)
{
	esp_now_peer_info_t info = {};
	memcpy(info.peer_addr, peer, sizeof(info.peer_addr));
	info.ifidx = WIFI_IF_STA;
	info.encrypt = encrypt;
	if (encrypt)
		memcpy(info.lmk, lmk, sizeof(info.lmk));
	esp_err_t result = esp_now_mod_peer(&info);
	if (result == ESP_OK)
	{
		esp_now_rate_config_t config = {};
		config.phymode = rates[rate].mode;
		config.rate = rates[rate].rate;
		result = esp_now_set_peer_rate_config(peer, &config);
	}
	if (result != ESP_OK)
		ESP_LOGW(tag, "%s %s: %s", rates[rate].name, keyName(encrypt), esp_err_to_name(result));
	return result == ESP_OK;
}

void Bench::addPeer(const uint8_t* const mac)
{
	if (esp_now_is_peer_exist(mac))
		return;
	esp_now_peer_info_t info = {};
	memcpy(info.peer_addr, mac, ESP_NOW_ETH_ALEN);
	info.ifidx = WIFI_IF_STA; /* channel 0: the current one, so the peer follows the responder's hops */
	ESP_ERROR_CHECK(esp_now_add_peer(&info));
}

uint8_t* Bench::prepare(const Type type, const uint8_t test, const uint32_t seq)
{
	Header& header = *reinterpret_cast<Header*>(txFrame);
	header.type = type;
	header.test = test;
	header.session = session;
	header.seq = seq;
	return txFrame + sizeof(Header);
}

bool Bench::sendFrame(const uint8_t* const mac, const uint8_t* const data, const size_t length)
{
	if (xSemaphoreTake(txSlots, pdMS_TO_TICKS(1000)) != pdTRUE)
	{
		ESP_LOGE(tag, "no send callback for 1 s");
		sendErrors++;
		return false;
	}
	for (int tries = 0;; ++tries)
	{
		const esp_err_t result = esp_now_send(mac, data, length);
		if (result == ESP_OK)
		{
			sent++;
			return true;
		}
		if (result == ESP_ERR_ESPNOW_NO_MEM && tries < 100)
		{
			noMem++;
			vTaskDelay(1);
			continue;
		}
		xSemaphoreGive(txSlots);
		sendErrors++;
		if (!errorLogged)
			ESP_LOGW(tag, "esp_now_send() of %u bytes: %s", length, esp_err_to_name(result));
		errorLogged = true;
		return false;
	}
}

void Bench::waitSent()
{
	for (int i = 0; i < CONFIG_ESPNOW_BENCH_WINDOW; ++i)
		xSemaphoreTake(txSlots, pdMS_TO_TICKS(1000));
	for (int i = 0; i < CONFIG_ESPNOW_BENCH_WINDOW; ++i)
		xSemaphoreGive(txSlots);
}

TxReport Bench::stream(const uint8_t test, const uint32_t durationMs, const size_t size)
{
	/* The callbacks of earlier frames must not count for this stream */
	waitSent();
	const uint32_t okBefore = macOk;
	const uint32_t failBefore = macFail;
	sent = 0;
	noMem = 0;
	sendErrors = 0;
	errorLogged = false;
	memset(txFrame + sizeof(Header), 0x5a, size - sizeof(Header));

	const int64_t end = esp_timer_get_time() + durationMs * 1000LL;
	for (uint32_t seq = 0; esp_timer_get_time() < end; ++seq)
	{
		prepare(Type::STREAM, test, seq);
		send(peer, size);
	}
	waitSent();
	return TxReport{sent, macOk - okBefore, macFail - failBefore, noMem, sendErrors};
}

void Bench::resetCount(const uint8_t test)
{
	counted = {};
	counted.active = true;
	counted.test = test;
	counted.dropsAtStart = queueDrops;
}

void Bench::count(const RxBuffer& buffer)
{
	const Header& header = headerOf(buffer);
	if (!counted.active || header.test != counted.test)
		resetCount(header.test);
	if (!counted.received)
		counted.firstUs = buffer.time;
	if (header.seq < counted.expected)
		counted.duplicates++;
	else
	{
		counted.gaps += header.seq - counted.expected;
		counted.expected = header.seq + 1;
	}
	counted.received++;
	counted.bytes += buffer.length;
	counted.lastUs = buffer.time;
	counted.rssiSum += buffer.rssi;
}

RxReport Bench::report(const uint8_t test) const
{
	RxReport rx = {};
	if (!counted.active || counted.test != test)
		return rx;
	rx.received = counted.received;
	rx.duplicates = counted.duplicates;
	rx.gaps = counted.gaps;
	rx.queueDrops = queueDrops - counted.dropsAtStart;
	rx.bytes = counted.bytes;
	rx.spanUs = static_cast<uint32_t>(counted.lastUs - counted.firstUs);
	rx.rssi = static_cast<int8_t>(counted.received ? counted.rssiSum / static_cast<int32_t>(counted.received) : 0);
	return rx;
}

Bench::RxBuffer* Bench::receive(const int64_t deadline)
{
	const int64_t remainingUs = deadline - esp_timer_get_time();
	const TickType_t ticks = remainingUs > 0 ? pdMS_TO_TICKS((remainingUs + 999) / 1000) : 0;
	RxBuffer* buffer = nullptr;
	return xQueueReceive(rxQueue, &buffer, ticks) == pdTRUE ? buffer : nullptr;
}

Bench::RxBuffer* Bench::waitFor(const Type type, const uint8_t test, const int64_t deadline)
{
	while (RxBuffer* const buffer = receive(deadline))
	{
		const Header& header = headerOf(*buffer);
		/* Before the responder is known, its HELLO_ACK is only matched by the session */
		if (header.type == type && header.test == test && header.session == session && (!hasPeer || isFromPeer(*buffer)))
			return buffer;
		release(buffer);
	}
	return nullptr;
}

Bench bench;
}  // namespace

extern "C" void app_main()
{
	bench.start();
}
