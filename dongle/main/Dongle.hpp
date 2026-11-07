/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: MIT
 */

#ifndef DONGLE_HPP
#define DONGLE_HPP

#include <stddef.h>
#include <stdint.h>

#include <atomic>

#include "EspNowLink.hpp"
#include "NetworkProbeProto.hpp"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "tinyusb_cdc_acm.h"

/**
 * @brief The ESP-NOW dongle: NETWORK PROBE frames between a host on the USB serial port and the probes in range.
 *
 * The host speaks the NETWORK PROBE protocol as over TCP. The dongle broadcasts its DISCOVER and IDENTIFY frames and
 * passes the answers back, remembering which probe answered from where; DONGLE_SELECT picks one and starts an
 * EspNowLink session with it, which every other frame then travels in, one frame per packet. The session lasts until
 * the host closes the port (DTR off), selects again, or the probe ends it or stops answering; the dongle keeps it alive
 * with PINGs.
 *
 * Delivery as EspNowLink asks: one packet in the air at a time, sent again while the radio gets no acknowledgement;
 * resends from the probe are dropped by their seq. Frames from the probe wait in the packet pool until the USB buffer
 * takes them, and the limit the dongle gives the probe counts the free part of the pool, so a host that reads slowly
 * holds the probe back instead of losing frames.
 *
 * One task does the work, woken by the USB and ESP-NOW callbacks; the status LED blinks blue without a session and
 * shows green with one, dipping for the traffic.
 */
class Dongle
{
   public:
	void start();

   private:
	struct Packet
	{
		uint8_t mac[6];
		uint16_t length;
		uint8_t data[EspNowLink::maxPacket];
	};

	/** A probe that answered DISCOVER. */
	struct Probe
	{
		char id[32];
		uint8_t mac[6];
		bool used;
	};

	enum class TxState : uint8_t
	{
		idle,
		inFlight,
		failed, /**< No acknowledgement: the packet goes again. */
	};

	static constexpr uint32_t poolSize = 32;
	static constexpr uint32_t poolReserve = 4; /**< Pool kept out of the probe's limit: discovery answers, CLOSE. */
	static constexpr size_t txQueueSize = 8;
	static constexpr uint16_t creditStep = 8; /**< The limit goes out in a PING of its own once it rose this much. */
	static constexpr size_t maxProbes = 16;
	static constexpr uint32_t taskStackSize = 6144;
	static constexpr uint32_t taskPriority = 5;
	static constexpr int taskCore = 1; /**< With TinyUSB; Wi-Fi has core 0. */
	static constexpr uint32_t pollMs = 50;
	static constexpr uint32_t hostPollMs = 1; /**< While frames wait for room in the USB buffer, which has no callback. */
	static constexpr TickType_t answerFlushTimeout = pdMS_TO_TICKS(20); /**< For room for an answer of the dongle's own. */

	static void taskEntry(void* arg);
	static void ledTaskEntry(void* arg);
	static void onReceive(const esp_now_recv_info_t* info, const uint8_t* data, int length);
	static void onSent(const esp_now_send_info_t* info, esp_now_send_status_t status);
	static void onUsbReceive(int itf, cdcacm_event_t* event);
	static void onLineState(int itf, cdcacm_event_t* event);

	[[noreturn]] void run();
	[[noreturn]] void runLed();
	void startWifi();
	void startUsb();
	void startLed();

	/* Host side */
	void readHost();
	void parseHost();
	void handleHostFrame(const NetworkProbe::FrameHeader& header, const uint8_t* frame, size_t length);
	void handleSelect(const NetworkProbe::FrameHeader& header, const uint8_t* payload);
	void answerHost(uint16_t seq, NetworkProbe::Type request, NetworkProbe::Status status);

	/**
	 * @brief Queues a whole frame for the host, if the USB buffer has room for it.
	 */
	bool writeHost(const uint8_t* frame, size_t length);

	/**
	 * @brief Moves the probe's frames that wait in the pool into the USB buffer, in order, while it has room.
	 */
	void flushHost();
	void serviceLine();

	/* Radio side */
	void handlePacket(Packet* packet);
	void rememberProbe(const uint8_t* mac, const uint8_t* frame, size_t length);
	void select(const Probe& probe);
	void deselect(bool tellProbe);
	void releasePacket(Packet* packet);

	/**
	 * @return The highest seq the probe may send: the last one taken, and the pool free for more.
	 */
	uint16_t limit() const;

	/**
	 * @brief Queues a packet to the probe of the session, with the limit in it.
	 */
	bool sendToProbe(EspNowLink::Kind kind, const uint8_t* frame, size_t length);
	bool queuePacket(const uint8_t* mac, const EspNowLink::Header& header, const uint8_t* frame, size_t length);

	/**
	 * @brief Sends the next queued packet once the one before is through, or sends that one again.
	 */
	void serviceTx();

	/**
	 * @brief Keeps the session alive, gives the probe a raised limit, and ends a session whose probe went silent.
	 */
	void serviceSession();

	static Dongle* instance;

	TaskHandle_t task = nullptr;
	QueueHandle_t freeQueue = nullptr; /**< Packet pointers. */
	QueueHandle_t rxQueue = nullptr;
	Packet pool[poolSize];
	std::atomic<uint32_t> radioDrops{0};
	std::atomic<bool> lineChanged{false};
	std::atomic<bool> hostOpen{false};

	/* Host frames: a partial one waits for the rest; one too long to send is skipped */
	uint8_t hostBuffer[EspNowLink::maxFrame];
	size_t hostLength = 0;
	size_t hostSkip = 0;
	uint32_t hostDrops = 0;

	/* The probe's frames waiting for room in the USB buffer, oldest first */
	Packet* hostQueue[poolSize] = {};
	size_t hostQueueHead = 0;
	size_t hostQueueCount = 0;

	/* Packets to send; the first one is in the air once sent */
	struct Outgoing
	{
		uint8_t mac[6];
		uint16_t length;
		uint8_t data[EspNowLink::maxPacket];
	};
	Outgoing txQueue[txQueueSize];
	size_t txHead = 0;
	size_t txCount = 0;
	bool txFrontSent = false;
	int txResends = 0;
	std::atomic<TxState> txState{TxState::idle};
	uint32_t txLost = 0;

	Probe probes[maxProbes] = {};
	size_t nextProbe = 0; /**< Taken when the table is full. */

	std::atomic<bool> selected{false};
	uint8_t probeMac[6] = {};
	uint16_t session = EspNowLink::noSession;
	uint16_t txSeq = 1;			  /**< seq of the next DATA to the probe. */
	uint16_t rxSeq = 0;			  /**< seq of the last DATA taken from the probe. */
	uint16_t advertisedLimit = 0; /**< The limit the probe got last. */
	int64_t lastSendUs = 0;
	std::atomic<int64_t> lastAckUs{0}; /**< The probe's radio acknowledged a packet. */

	led_strip_handle_t led = nullptr;
	std::atomic<uint32_t> traffic{0};
	std::atomic<bool> alert{false}; /**< The session ended from the probe's side: one red flash. */
};

#endif /* DONGLE_HPP */
