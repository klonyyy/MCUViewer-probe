/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Application.hpp"

#include "esp_log.h"
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
}  // namespace

void App::init()
{
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
	netServer.start(hostLink, gdbServer.getListenSocket(), swoCapture);

	while (true)
	{
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		probeService.service();

		// Check if GDB has been requested by the host
		if (hostLink.gdbRequested.load(std::memory_order_acquire))
		{
			hostLink.gdbActive.store(true, std::memory_order_release);
			gdbServer.serve();
			hostLink.gdbActive.store(false, std::memory_order_release);
			hostLink.gdbRequested.store(false, std::memory_order_release);
			hostLink.wakeNet();
		}
	}
}
