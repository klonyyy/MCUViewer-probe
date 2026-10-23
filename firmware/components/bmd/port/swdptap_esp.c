/*
 * SPDX-FileCopyrightText: 2026 MVProbe contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * SWD bit-bang tap for the ESP32-S3 using dedicated GPIO (CPU GPIO instructions).
 *
 * Timing follows the convention of BMD's generic tap (src/platforms/common/swdptap.c):
 * - every primitive ends with a falling SWCLK edge and assumes one immediately preceded it,
 * - the probe changes SWDIO together with the falling edge,
 * - SWDIO is sampled right before the rising edge.
 *
 * Half periods are timed against the CPU cycle counter. After an interrupt the schedule is
 * resynchronised instead of caught up, so a delay never produces a burst of short pulses.
 *
 * SWDIO direction is switched through GPIO_ENABLE_W1TS/W1TC. For that to work, the output
 * enable of both pins is taken away from the dedicated GPIO peripheral at init.
 */

#include "general.h"
#include "swd.h"
#include "bmd_port.h"

#include "driver/dedic_gpio.h"
#include "driver/gpio.h"
#include "hal/dedic_gpio_cpu_ll.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "sdkconfig.h"

#define SWCLK_GPIO CONFIG_MVPROBE_SWCLK_GPIO
#define SWDIO_GPIO CONFIG_MVPROBE_SWDIO_GPIO

#define CALIBRATION_CLOCKS 256U

static const char *const TAG = "swd_tap";

swd_proc_s swd_proc;

static dedic_gpio_bundle_handle_t swd_bundle;
/* Dedicated GPIO channel masks */
static uint32_t clk_mask;
static uint32_t dio_mask;
static uint32_t dio_in_mask;

static uint32_t cpu_frequency;
/* CPU cycles per SWCLK half period, 0 = as fast as the loop runs */
static uint32_t half_period;
/* Fastest SWCLK the loop can produce, measured at init */
static uint32_t max_frequency;
static bool swdio_driven;
static swd_tap_stats_s stats;

static uint32_t swdptap_seq_in(size_t clock_cycles);
static bool swdptap_seq_in_parity(uint32_t *ret, size_t clock_cycles);
static void swdptap_seq_out(uint32_t tms_states, size_t clock_cycles);
static void swdptap_seq_out_parity(uint32_t tms_states, size_t clock_cycles);

/* Wait until one half period after the previous edge, return the time of the new edge */
static inline __attribute__((always_inline)) uint32_t swd_delay(uint32_t edge, const uint32_t half)
{
	edge += half;
	uint32_t now;
	do {
		now = esp_cpu_get_cycle_count();
	} while ((int32_t)(now - edge) < 0);
	if (now - edge > (half >> 1U))
		edge = now;
	return edge;
}

static inline __attribute__((always_inline)) void swd_stats_add(const uint32_t start, const size_t clocks)
{
	stats.cycles += esp_cpu_get_cycle_count() - start;
	stats.clocks += clocks;
}

static void IRAM_ATTR swd_seq_out_raw(uint32_t value, const size_t clock_cycles, const uint32_t half)
{
	uint32_t edge = esp_cpu_get_cycle_count();
	for (size_t cycle = 0; cycle < clock_cycles; ++cycle) {
		dedic_gpio_cpu_ll_write_mask(clk_mask | dio_mask, (value & 1U) ? dio_mask : 0U);
		value >>= 1U;
		edge = swd_delay(edge, half);
		dedic_gpio_cpu_ll_write_mask(clk_mask, clk_mask);
		edge = swd_delay(edge, half);
	}
	dedic_gpio_cpu_ll_write_mask(clk_mask, 0U);
}

static uint32_t IRAM_ATTR swd_seq_in_raw(const size_t clock_cycles, const uint32_t half)
{
	uint32_t edge = esp_cpu_get_cycle_count();
	uint32_t value = 0;
	for (size_t cycle = 0; cycle < clock_cycles; ++cycle) {
		edge = swd_delay(edge, half);
		const uint32_t bit = (dedic_gpio_cpu_ll_read_in() & dio_in_mask) ? 1U : 0U;
		dedic_gpio_cpu_ll_write_mask(clk_mask, clk_mask);
		value |= bit << cycle;
		edge = swd_delay(edge, half);
		dedic_gpio_cpu_ll_write_mask(clk_mask, 0U);
	}
	return value;
}

/* Returns the number of clock cycles spent (0 or 1) */
static size_t IRAM_ATTR swd_turnaround(const bool drive, const uint32_t half)
{
	if (drive == swdio_driven)
		return 0;
	swdio_driven = drive;

	if (!drive)
		GPIO.enable_w1tc = 1U << SWDIO_GPIO;
	uint32_t edge = esp_cpu_get_cycle_count();
	edge = swd_delay(edge, half);
	dedic_gpio_cpu_ll_write_mask(clk_mask, clk_mask);
	edge = swd_delay(edge, half);
	dedic_gpio_cpu_ll_write_mask(clk_mask, 0U);
	if (drive)
		GPIO.enable_w1ts = 1U << SWDIO_GPIO;
	return 1U;
}

static uint32_t IRAM_ATTR swdptap_seq_in(const size_t clock_cycles)
{
	const uint32_t start = esp_cpu_get_cycle_count();
	const uint32_t half = half_period;
	const size_t turnaround = swd_turnaround(false, half);
	const uint32_t value = swd_seq_in_raw(clock_cycles, half);
	swd_stats_add(start, clock_cycles + turnaround);
	return value;
}

static bool IRAM_ATTR swdptap_seq_in_parity(uint32_t *const ret, const size_t clock_cycles)
{
	const uint32_t start = esp_cpu_get_cycle_count();
	const uint32_t half = half_period;
	size_t turnaround = swd_turnaround(false, half);
	const uint32_t value = swd_seq_in_raw(clock_cycles, half);
	const uint32_t parity = swd_seq_in_raw(1U, half);
	turnaround += swd_turnaround(true, half);
	swd_stats_add(start, clock_cycles + 1U + turnaround);
	*ret = value;
	return (uint32_t)__builtin_parity(value) == parity;
}

static void IRAM_ATTR swdptap_seq_out(const uint32_t tms_states, const size_t clock_cycles)
{
	const uint32_t start = esp_cpu_get_cycle_count();
	const uint32_t half = half_period;
	const size_t turnaround = swd_turnaround(true, half);
	swd_seq_out_raw(tms_states, clock_cycles, half);
	swd_stats_add(start, clock_cycles + turnaround);
}

static void IRAM_ATTR swdptap_seq_out_parity(const uint32_t tms_states, const size_t clock_cycles)
{
	const uint32_t start = esp_cpu_get_cycle_count();
	const uint32_t half = half_period;
	const size_t turnaround = swd_turnaround(true, half);
	swd_seq_out_raw(tms_states, clock_cycles, half);
	swd_seq_out_raw((uint32_t)__builtin_parity(tms_states), 1U, half);
	swd_stats_add(start, clock_cycles + 1U + turnaround);
}

void swdptap_init(void)
{
	swd_proc.seq_in = swdptap_seq_in;
	swd_proc.seq_in_parity = swdptap_seq_in_parity;
	swd_proc.seq_out = swdptap_seq_out;
	swd_proc.seq_out_parity = swdptap_seq_out_parity;
}

void swd_tap_drive_enable(const bool enable)
{
	if (enable) {
		/* SWCLK low, SWDIO high (idle line state) */
		dedic_gpio_cpu_ll_write_mask(clk_mask | dio_mask, dio_mask);
		GPIO.enable_w1ts = (1U << SWCLK_GPIO) | (1U << SWDIO_GPIO);
		swdio_driven = true;
	} else {
		GPIO.enable_w1tc = (1U << SWCLK_GPIO) | (1U << SWDIO_GPIO);
		swdio_driven = false;
	}
}

/* Run the output loop at full speed with both pins released, to find the fastest clock */
static uint32_t swd_tap_calibrate(void)
{
	const uint32_t start = esp_cpu_get_cycle_count();
	swd_seq_out_raw(0x55555555U, CALIBRATION_CLOCKS, 0U);
	const uint32_t cycles = esp_cpu_get_cycle_count() - start;
	return (uint32_t)((uint64_t)CALIBRATION_CLOCKS * cpu_frequency / cycles);
}

void swd_tap_hw_init(void)
{
	const int gpios[] = {SWCLK_GPIO, SWDIO_GPIO};
	const dedic_gpio_bundle_config_t config = {
		.gpio_array = gpios,
		.array_size = ARRAY_LENGTH(gpios),
		.flags = {
			.in_en = 1,
			.out_en = 1,
		},
	};
	ESP_ERROR_CHECK(dedic_gpio_new_bundle(&config, &swd_bundle));

	uint32_t out_offset = 0;
	uint32_t in_offset = 0;
	ESP_ERROR_CHECK(dedic_gpio_get_out_offset(swd_bundle, &out_offset));
	ESP_ERROR_CHECK(dedic_gpio_get_in_offset(swd_bundle, &in_offset));
	clk_mask = 1U << out_offset;
	dio_mask = 1U << (out_offset + 1U);
	dio_in_mask = 1U << (in_offset + 1U);

	/* Output enables come from GPIO_ENABLE_REG, so swd_turnaround() can switch SWDIO */
	gpio_ll_set_output_enable_ctrl(&GPIO, SWCLK_GPIO, false, false);
	gpio_ll_set_output_enable_ctrl(&GPIO, SWDIO_GPIO, false, false);
	/* Keep a released SWDIO at a defined level */
	gpio_pullup_en(SWDIO_GPIO);
	swd_tap_drive_enable(false);

	cpu_frequency = esp_rom_get_cpu_ticks_per_us() * 1000000U;
	max_frequency = swd_tap_calibrate();
	swdptap_init();

	ESP_LOGI(TAG, "SWCLK GPIO%d, SWDIO GPIO%d on core %d, max SWCLK %" PRIu32 " Hz", SWCLK_GPIO, SWDIO_GPIO,
		esp_cpu_get_core_id(), max_frequency);
}

void swd_tap_set_frequency(uint32_t frequency)
{
	if (frequency >= max_frequency) {
		half_period = 0;
		return;
	}
	if (frequency < 1000U)
		frequency = 1000U;
	/* Round up, so the clock never exceeds the request */
	half_period = (cpu_frequency + 2U * frequency - 1U) / (2U * frequency);
}

uint32_t swd_tap_get_frequency(void)
{
	if (!half_period)
		return max_frequency;
	return MIN(cpu_frequency / (2U * half_period), max_frequency);
}

uint32_t swd_tap_half_period_cycles(void)
{
	return half_period;
}

uint32_t swd_tap_cpu_frequency(void)
{
	return cpu_frequency;
}

uint32_t swd_tap_cycle_count(void)
{
	return esp_cpu_get_cycle_count();
}

void swd_tap_stats_get(swd_tap_stats_s *const out)
{
	*out = stats;
}

void swd_tap_stats_reset(void)
{
	stats.clocks = 0;
	stats.cycles = 0;
}
