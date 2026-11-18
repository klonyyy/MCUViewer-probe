/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Application.hpp"

#include "StatusLed.hpp"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

extern "C"
{
#include "general.h"
#include "platform.h"
}

App app;

namespace
{
constexpr const char* tag = "network_probe";

const char* resetReasonName(const esp_reset_reason_t reason)
{
	switch (reason)
	{
		case ESP_RST_POWERON:
			return "power-on";
		case ESP_RST_EXT:
			return "the reset pin";
		case ESP_RST_SW:
			return "a software restart";
		case ESP_RST_PANIC:
			return "a panic";
		case ESP_RST_INT_WDT:
			return "the interrupt watchdog";
		case ESP_RST_TASK_WDT:
			return "the task watchdog";
		case ESP_RST_WDT:
			return "a watchdog";
		case ESP_RST_BROWNOUT:
			return "a brownout: the supply voltage dropped";
		case ESP_RST_USB:
			return "a USB reset";
		case ESP_RST_JTAG:
			return "JTAG";
		case ESP_RST_PWR_GLITCH:
			return "a power glitch";
		case ESP_RST_CPU_LOCKUP:
			return "a CPU lockup";
		default:
			return "an unknown reason";
	}
}
}  // namespace

void App::init()
{
	/* First, so the LED shows the network coming up */
	StatusLed::start();

	/* A probe powered by its target restarts with it, or browns out when the radio draws more than the supply gives */
	const esp_reset_reason_t reason = esp_reset_reason();
	ESP_LOGI(tag, "started after %s (reset reason %d)", resetReasonName(reason), static_cast<int>(reason));

	esp_err_t result = nvs_flash_init();
	if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND)
	{
		ESP_ERROR_CHECK(nvs_flash_erase());
		result = nvs_flash_init();
	}
	ESP_ERROR_CHECK(result);

	wifi.start();

	ESP_LOGI(tag, "starting SWD owner task on core %d", probeTaskCore);
	xTaskCreatePinnedToCore(probeTask, "probe", probeTaskStackSize, this, probeTaskPriority, nullptr, probeTaskCore);
}

void App::probeTask(void* arg)
{
	static_cast<App*>(arg)->runProbe();
}

void App::runProbe()
{
	/* Must run on the core that owns SWD: it creates the dedicated GPIO bundle */
	platform_init();
	gdbServer.init();
	hostLink.probeTask = xTaskGetCurrentTaskHandle();
	probeService.init(hostLink);
	netServer.start(hostLink, gdbServer.getListenSocket(), swoCapture, wifi);

	while (true)
	{
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		probeService.service();

		// Check if GDB has been requested by the host
		if (hostLink.gdbRequested.load(std::memory_order_acquire))
		{
			hostLink.gdbActive.store(true, std::memory_order_release);
			StatusLed::setGdbConnected(true);
			gdbServer.serve();
			StatusLed::setGdbConnected(false);
			hostLink.gdbActive.store(false, std::memory_order_release);
			hostLink.gdbRequested.store(false, std::memory_order_release);
			hostLink.wakeNet();
		}
	}
}
