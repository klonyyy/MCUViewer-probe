/*
 * SPDX-FileCopyrightText: 2026 MVProbe contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * MVProbe: wireless SWD probe for MCUViewer, built on Black Magic Debug.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "gdb_server.h"
#include "wifi.h"

/* gdb_main() and the ROM table walk use alloca() and recursion */
#define PROBE_TASK_STACK_SIZE 12288U
#define PROBE_TASK_PRIORITY   5U
/* Wi-Fi and lwIP are pinned to core 0, SWD gets core 1 to itself */
#define PROBE_TASK_CORE 1

static const char *const TAG = "mvprobe";

static void probe_task(void *arg)
{
	(void)arg;
	probe_task_main();
}

void app_main(void)
{
	esp_err_t result = nvs_flash_init();
	if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		result = nvs_flash_init();
	}
	ESP_ERROR_CHECK(result);

	wifi_start();

	ESP_LOGI(TAG, "starting SWD owner task on core %d", PROBE_TASK_CORE);
	xTaskCreatePinnedToCore(
		probe_task, "probe", PROBE_TASK_STACK_SIZE, NULL, PROBE_TASK_PRIORITY, NULL, PROBE_TASK_CORE);
}
