/*
 * SPDX-FileCopyrightText: 2026 MVProbe contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Application.hpp"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

App app;

namespace
{
constexpr const char* tag = "mvprobe";
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
	static_cast<App*>(arg)->gdbServer.run();
}
