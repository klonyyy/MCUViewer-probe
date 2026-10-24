/*
 * SPDX-FileCopyrightText: 2026 MVProbe contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Black Magic Debug platform functions for the ESP32-S3 port.
 */

#include "general.h"
#include "platform.h"
#include "bmd_port.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#define NRST_GPIO CONFIG_MVPROBE_NRST_GPIO

/* Declared by timing.h for BMD's generic bit-bang taps; the ESP tap keeps its own timing */
uint32_t target_clk_divider = 0;

/* nRST has no sense line, so report the state we last drove */
static bool nrst_asserted = false;

void platform_init(void)
{
	swd_tap_hw_init();
	swd_tap_set_frequency(CONFIG_MVPROBE_SWD_FREQ_HZ);

#if NRST_GPIO >= 0
	const gpio_config_t nrst_config = {
		.pin_bit_mask = 1ULL << NRST_GPIO,
		.mode = GPIO_MODE_INPUT_OUTPUT_OD,
		.pull_up_en = GPIO_PULLUP_ENABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	gpio_set_level(NRST_GPIO, 1);
	gpio_config(&nrst_config);
#endif
}

uint32_t platform_time_ms(void)
{
	return (uint32_t)(esp_timer_get_time() / 1000);
}

void platform_delay(const uint32_t ms)
{
	/* vTaskDelay(n) may return up to one tick early, so add one */
	if (ms)
		vTaskDelay(pdMS_TO_TICKS(ms) + 1U);
}

void platform_nrst_set_val(const bool assert)
{
#if NRST_GPIO >= 0
	gpio_set_level(NRST_GPIO, assert ? 0 : 1);
#endif
	nrst_asserted = assert;
}

bool platform_nrst_get_val(void)
{
	return nrst_asserted;
}

const char *platform_target_voltage(void)
{
	return "Unknown";
}

int platform_hwversion(void)
{
	return 0;
}

void platform_max_frequency_set(const uint32_t frequency)
{
	swd_tap_set_frequency(frequency);
}

uint32_t platform_max_frequency_get(void)
{
	return swd_tap_get_frequency();
}

void platform_target_clk_output_enable(const bool enable)
{
	swd_tap_drive_enable(enable);
}

/* Target semihosting output redirected away from GDB goes to the probe console */
void debug_serial_send_stdout(const uint8_t *const data, const size_t len)
{
	fwrite(data, 1, len, stdout);
	fflush(stdout);
}

/* No on-board SPI flash to expose through "monitor spi_scan" */
bool platform_spi_init(const spi_bus_e bus)
{
	(void)bus;
	return false;
}

bool platform_spi_deinit(const spi_bus_e bus)
{
	(void)bus;
	return false;
}

bool platform_spi_chip_select(const uint8_t device_select)
{
	(void)device_select;
	return false;
}

uint8_t platform_spi_xfer(const spi_bus_e bus, const uint8_t value)
{
	(void)bus;
	(void)value;
	return 0xffU;
}
