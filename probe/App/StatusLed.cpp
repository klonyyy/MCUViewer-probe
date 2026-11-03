/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "StatusLed.hpp"

#include <math.h>

#include <algorithm>
#include <atomic>

#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace
{
constexpr const char* tag = "status_led";

constexpr uint32_t taskStackSize = 3072;
constexpr uint32_t taskPriority = 2; /**< Below the net and Wi-Fi tasks: a late LED frame costs nothing. */
constexpr int taskCore = 0;			 /**< With Wi-Fi and lwIP; core 1 belongs to SWD. */
constexpr uint32_t tickMs = 20;

/* WS2812 bits at 10 MHz: a 0 is 0.3 us high and 0.9 us low, a 1 the other way round */
constexpr uint32_t rmtResolutionHz = 10000000;
constexpr uint16_t shortTicks = 3;
constexpr uint16_t longTicks = 9;
constexpr size_t rmtMemSymbols = 48; /**< One RMT memory block of the ESP32-S3; a frame takes 24 symbols. */
constexpr int txTimeoutMs = 10;

constexpr int64_t activityDipMs = 40;  /**< A dip for traffic... */
constexpr int64_t activityGapMs = 100; /**< ...at most this often, so a stream of frames flickers at 10 Hz. */
constexpr float activityLevel = 0.05f;

constexpr int64_t identifyMs = 2000;

struct Color
{
	uint8_t r;
	uint8_t g;
	uint8_t b;

	bool operator==(const Color&) const = default;
};

/**
 * @brief A color at a level from 0 to 1, linear in LED current.
 */
struct Look
{
	Color color;
	float level;
};

constexpr Color black = {0, 0, 0};
constexpr Color blue = {0, 40, 255};
constexpr Color purple = {150, 0, 255};
constexpr Color green = {0, 255, 24};
constexpr Color cyan = {0, 170, 255};
constexpr Color orange = {255, 64, 0};
constexpr Color amber = {255, 140, 0};
constexpr Color red = {255, 0, 0};
constexpr Color white = {255, 255, 255};

/**
 * @brief An alert: count blinks, onMs of every periodMs lit.
 */
struct AlertLook
{
	Color color;
	int64_t onMs;
	int64_t periodMs;
	int64_t count;
};

/* Indexed by StatusLed::Alert */
constexpr AlertLook alertLooks[] = {
	{black, 0, 1, 0},	  /* none */
	{amber, 120, 250, 1}, /* warning */
	{red, 120, 250, 1},	  /* error */
	{red, 150, 300, 4},	  /* failure */
};

/* Any task */
std::atomic<StatusLed::Network> network{StatusLed::Network::connecting};
std::atomic<bool> hostConnected{false};
std::atomic<bool> gdbConnected{false};
std::atomic<uint32_t> buttonRemainingMs{0};
std::atomic<bool> activityPending{false};
std::atomic<bool> identifyPending{false};
std::atomic<StatusLed::Alert> pendingAlert{StatusLed::Alert::none}; /**< The most severe since the LED task looked. */

/* LED task */
rmt_channel_handle_t channel = nullptr;
rmt_encoder_handle_t encoder = nullptr;
uint8_t frame[3]; /**< Must stay as it is until RMT has sent it. */
StatusLed::Alert activeAlert = StatusLed::Alert::none;
int64_t alertStart = 0;
int64_t activityDipEnd = 0;
int64_t nextActivity = 0;
int64_t identifyStart = 0;
int64_t identifyEnd = 0;
float holdPhase = 0.0f; /**< Of the button hold blinks, 0 to 1. */

/* A WS2812 bit: high, then low */
rmt_symbol_word_t bitSymbol(const uint16_t highTicks, const uint16_t lowTicks)
{
	rmt_symbol_word_t symbol = {};
	symbol.level0 = 1;
	symbol.duration0 = highTicks;
	symbol.level1 = 0;
	symbol.duration1 = lowTicks;
	return symbol;
}

void show(const Color color)
{
#if CONFIG_NETWORK_PROBE_LED_ORDER_RGB
	frame[0] = color.r;
	frame[1] = color.g;
#else
	frame[0] = color.g;
	frame[1] = color.r;
#endif
	frame[2] = color.b;
	const rmt_transmit_config_t transmitConfig = {};
	if (rmt_transmit(channel, encoder, frame, sizeof(frame), &transmitConfig) == ESP_OK)
		rmt_tx_wait_all_done(channel, txTimeoutMs);
}

bool open()
{
	const rmt_tx_channel_config_t channelConfig = {
		.gpio_num = static_cast<gpio_num_t>(CONFIG_NETWORK_PROBE_LED_GPIO),
		.clk_src = RMT_CLK_SRC_DEFAULT,
		.resolution_hz = rmtResolutionHz,
		.mem_block_symbols = rmtMemSymbols,
		.trans_queue_depth = 1,
		.intr_priority = 0,
		.flags = {},
	};
	esp_err_t result = rmt_new_tx_channel(&channelConfig, &channel);

	rmt_bytes_encoder_config_t encoderConfig = {};
	encoderConfig.bit0 = bitSymbol(shortTicks, longTicks);
	encoderConfig.bit1 = bitSymbol(longTicks, shortTicks);
	encoderConfig.flags.msb_first = 1;
	if (result == ESP_OK)
		result = rmt_new_bytes_encoder(&encoderConfig, &encoder);
	if (result == ESP_OK)
		result = rmt_enable(channel);
	if (result != ESP_OK)
	{
		ESP_LOGE(tag, "cannot drive the LED on GPIO%d: %s, status LED disabled", CONFIG_NETWORK_PROBE_LED_GPIO, esp_err_to_name(result));
		return false;
	}

	/* A restart leaves the LED lit as it was */
	show(black);
	ESP_LOGI(tag, "status LED on GPIO%d", CONFIG_NETWORK_PROBE_LED_GPIO);
	return true;
}

/* From floor to 1 and back over periodMs, eased like breathing */
float breathing(const int64_t nowMs, const int64_t periodMs, const float floor)
{
	const float phase = static_cast<float>(nowMs % periodMs) / static_cast<float>(periodMs);
	const float perceived = floor + (1.0f - floor) * (0.5f - 0.5f * cosf(2.0f * static_cast<float>(M_PI) * phase));
	/* The eye sees roughly the square root of the light, so square it for an even swell */
	return perceived * perceived;
}

float blink(const int64_t nowMs, const int64_t onMs, const int64_t periodMs)
{
	return nowMs % periodMs < onMs ? 1.0f : 0.0f;
}

void updateAlert(const int64_t nowMs)
{
	if (activeAlert != StatusLed::Alert::none)
	{
		const AlertLook& look = alertLooks[static_cast<size_t>(activeAlert)];
		if (nowMs - alertStart >= look.periodMs * look.count)
			activeAlert = StatusLed::Alert::none;
	}
	const StatusLed::Alert next = pendingAlert.exchange(StatusLed::Alert::none, std::memory_order_relaxed);
	if (next > activeAlert)
	{
		activeAlert = next;
		alertStart = nowMs;
	}
}

void updateActivity(const int64_t nowMs)
{
	/* Left pending during a gap: the dip comes late rather than not at all */
	if (nowMs >= nextActivity && activityPending.exchange(false, std::memory_order_relaxed))
	{
		activityDipEnd = nowMs + activityDipMs;
		nextActivity = nowMs + activityGapMs;
	}
}

void updateIdentify(const int64_t nowMs)
{
	if (!identifyPending.exchange(false, std::memory_order_relaxed))
		return;
	/* A request while it blinks makes it longer without breaking the rhythm */
	if (nowMs >= identifyEnd)
		identifyStart = nowMs;
	identifyEnd = nowMs + identifyMs;
}

Look buttonLook(const uint32_t remainingMs)
{
	/* Twice a second when pressed, about 12 times a second at the end */
	const float periodMs = std::clamp(static_cast<float>(remainingMs) / 6.0f, 80.0f, 500.0f);
	holdPhase += static_cast<float>(tickMs) / periodMs;
	holdPhase -= floorf(holdPhase);
	return {white, holdPhase < 0.5f ? 1.0f : 0.0f};
}

Look sessionLook(const int64_t nowMs, const Color color)
{
	return {color, nowMs < activityDipEnd ? activityLevel : 1.0f};
}

Look networkLook(const int64_t nowMs)
{
	switch (network.load(std::memory_order_relaxed))
	{
		case StatusLed::Network::connecting:
			return {blue, blink(nowMs, 100, 500)};
		case StatusLed::Network::setup:
			return {purple, breathing(nowMs, 2000, 0.1f)};
		case StatusLed::Network::online:
			break;
	}
	return {green, breathing(nowMs, 4000, 0.15f)};
}

Look render(const int64_t nowMs)
{
	/* Alerts, traffic and identification are taken in every frame, also while something else is shown */
	updateAlert(nowMs);
	updateActivity(nowMs);
	updateIdentify(nowMs);

	const uint32_t remainingMs = buttonRemainingMs.load(std::memory_order_relaxed);
	if (remainingMs)
		return buttonLook(remainingMs);
	holdPhase = 0.0f;

	if (nowMs < identifyEnd)
		return {white, blink(nowMs - identifyStart, 100, 200)};

	if (activeAlert != StatusLed::Alert::none)
	{
		const AlertLook& look = alertLooks[static_cast<size_t>(activeAlert)];
		return {look.color, blink(nowMs - alertStart, look.onMs, look.periodMs)};
	}
	if (gdbConnected.load(std::memory_order_relaxed))
		return sessionLook(nowMs, orange);
	if (hostConnected.load(std::memory_order_relaxed))
		return sessionLook(nowMs, cyan);
	return networkLook(nowMs);
}

uint8_t scale(const uint8_t value, const float level)
{
	return static_cast<uint8_t>(static_cast<float>(value) * level * (CONFIG_NETWORK_PROBE_LED_BRIGHTNESS / 255.0f) + 0.5f);
}

void ledTask(void*)
{
	/* Here, not in start(): the RMT interrupt is allocated on the calling core */
	if (!open())
		vTaskDelete(nullptr);

	Color shown = black;
	TickType_t wake = xTaskGetTickCount();
	while (true)
	{
		const Look look = render(esp_timer_get_time() / 1000);
		const Color color = {scale(look.color.r, look.level), scale(look.color.g, look.level), scale(look.color.b, look.level)};
		if (color != shown)
		{
			show(color);
			shown = color;
		}
		xTaskDelayUntil(&wake, pdMS_TO_TICKS(tickMs));
	}
}
}  // namespace

void StatusLed::start()
{
#if CONFIG_NETWORK_PROBE_LED_GPIO >= 0
	xTaskCreatePinnedToCore(ledTask, "led", taskStackSize, nullptr, taskPriority, nullptr, taskCore);
#endif
}

void StatusLed::setNetwork(const Network state)
{
	network.store(state, std::memory_order_relaxed);
}

void StatusLed::setHostConnected(const bool connected)
{
	hostConnected.store(connected, std::memory_order_relaxed);
}

void StatusLed::setGdbConnected(const bool connected)
{
	gdbConnected.store(connected, std::memory_order_relaxed);
}

void StatusLed::setButtonHold(const uint32_t remainingMs)
{
	buttonRemainingMs.store(remainingMs, std::memory_order_relaxed);
}

void StatusLed::identify()
{
	identifyPending.store(true, std::memory_order_relaxed);
}

void StatusLed::activity()
{
	activityPending.store(true, std::memory_order_relaxed);
}

void StatusLed::alert(const Alert alert)
{
	/* Several tasks may raise one at a time: keep the most severe */
	Alert pending = pendingAlert.load(std::memory_order_relaxed);
	while (pending < alert && !pendingAlert.compare_exchange_weak(pending, alert, std::memory_order_relaxed))
	{
	}
}
