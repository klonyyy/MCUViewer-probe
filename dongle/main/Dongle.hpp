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
 * the host closes the port (DTR off), selects again, or the probe ends it; the dongle keeps it alive with PINGs.
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

	static constexpr uint32_t poolSize = 32; /**< Packets for the host: the radio outruns USB, this bridges a burst. */
	static constexpr size_t maxProbes = 16;
	static constexpr uint32_t taskStackSize = 6144;
	static constexpr uint32_t taskPriority = 5;
	static constexpr int taskCore = 1; /**< With TinyUSB; Wi-Fi has core 0. */
	static constexpr uint32_t pollMs = 50;
	static constexpr TickType_t hostFlushTimeout = pdMS_TO_TICKS(20); /**< For room in the USB buffer before a frame is dropped. */

	static void taskEntry(void* arg);
	static void ledTaskEntry(void* arg);
	static void onReceive(const esp_now_recv_info_t* info, const uint8_t* data, int length);
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
	 * @brief Queues a whole frame for the host, or drops it when the USB buffer stays full.
	 */
	void writeHost(const uint8_t* frame, size_t length);
	void serviceLine();

	/* Radio side */
	void handlePacket(const Packet& packet);
	void rememberProbe(const uint8_t* mac, const uint8_t* frame, size_t length);
	void select(const Probe& probe);
	void deselect(bool tellProbe);
	bool sendToProbe(EspNowLink::Kind kind, const uint8_t* frame, size_t length);
	bool sendPacket(const uint8_t* mac, EspNowLink::Kind kind, uint16_t session, const uint8_t* frame, size_t length);

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

	Probe probes[maxProbes] = {};
	size_t nextProbe = 0; /**< Taken when the table is full. */

	std::atomic<bool> selected{false};
	uint8_t probeMac[6] = {};
	uint16_t session = EspNowLink::noSession;
	int64_t lastSendUs = 0;
	uint8_t txPacket[EspNowLink::maxPacket];

	led_strip_handle_t led = nullptr;
	std::atomic<uint32_t> traffic{0};
	std::atomic<bool> alert{false}; /**< The probe ended the session: one red flash. */
};

#endif /* DONGLE_HPP */
