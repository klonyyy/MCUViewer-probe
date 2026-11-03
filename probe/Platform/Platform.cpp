/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Black Magic Debug platform functions for the ESP32-S3 port.
 */

#include <stdio.h>

#include "SwdTap.hpp"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

extern "C"
{
#include "general.h"
#include "platform.h"
}

namespace
{
constexpr gpio_num_t nrstGpio = static_cast<gpio_num_t>(CONFIG_NETWORK_PROBE_NRST_GPIO); /**< GPIO_NUM_NC (-1) when not connected. */

/* nRST has no sense line, so report the state we last drove */
bool nrstAsserted = false;
}  // namespace

/* Declared by timing.h for BMD's generic bit-bang taps; the ESP tap keeps its own timing */
uint32_t target_clk_divider = 0;

void platform_init(void)
{
	SwdTap::init();
	SwdTap::setFrequency(CONFIG_NETWORK_PROBE_SWD_FREQ_HZ);

#if CONFIG_NETWORK_PROBE_NRST_GPIO >= 0
	const gpio_config_t nrstConfig = {
		.pin_bit_mask = 1ULL << nrstGpio,
		.mode = GPIO_MODE_INPUT_OUTPUT_OD,
		.pull_up_en = GPIO_PULLUP_ENABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	gpio_set_level(nrstGpio, 1);
	gpio_config(&nrstConfig);
#endif
}

uint32_t platform_time_ms(void)
{
	return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

void platform_delay(const uint32_t ms)
{
	/* vTaskDelay(n) may return up to one tick early, so add one */
	if (ms)
		vTaskDelay(pdMS_TO_TICKS(ms) + 1U);
}

void platform_nrst_set_val(const bool assert)
{
#if CONFIG_NETWORK_PROBE_NRST_GPIO >= 0
	gpio_set_level(nrstGpio, assert ? 0 : 1);
#endif
	nrstAsserted = assert;
}

bool platform_nrst_get_val(void)
{
	return nrstAsserted;
}

const char* platform_target_voltage(void)
{
	return "Unknown";
}

int platform_hwversion(void)
{
	return 0;
}

void platform_max_frequency_set(const uint32_t frequency)
{
	SwdTap::setFrequency(frequency);
}

uint32_t platform_max_frequency_get(void)
{
	return SwdTap::getFrequency();
}

void platform_target_clk_output_enable(const bool enable)
{
	SwdTap::setDriveEnabled(enable);
}

/* Target semihosting output redirected away from GDB goes to the probe console */
void debug_serial_send_stdout(const uint8_t* const data, const size_t len)
{
	fwrite(data, 1, len, stdout);
	fflush(stdout);
}

/* No on-board SPI flash to expose through "monitor spi_scan" */
bool platform_spi_init([[maybe_unused]] const spi_bus_e bus)
{
	return false;
}

bool platform_spi_deinit([[maybe_unused]] const spi_bus_e bus)
{
	return false;
}

bool platform_spi_chip_select([[maybe_unused]] const uint8_t device_select)
{
	return false;
}

uint8_t platform_spi_xfer([[maybe_unused]] const spi_bus_e bus, [[maybe_unused]] const uint8_t value)
{
	return 0xffU;
}
