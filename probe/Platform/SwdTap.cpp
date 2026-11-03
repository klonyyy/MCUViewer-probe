/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
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

#include "SwdTap.hpp"

#include <algorithm>
#include <iterator>

#include "driver/dedic_gpio.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "hal/dedic_gpio_cpu_ll.h"
#include "hal/gpio_ll.h"
#include "sdkconfig.h"
#include "soc/gpio_struct.h"

extern "C"
{
#include "general.h"
#include "swd.h"
}

namespace
{
constexpr int swclkGpio = CONFIG_NETWORK_PROBE_SWCLK_GPIO;
constexpr int swdioGpio = CONFIG_NETWORK_PROBE_SWDIO_GPIO;

constexpr size_t calibrationClocks = 256;

constexpr const char* tag = "swd_tap";

/* Wait until one half period after the previous edge, return the time of the new edge */
[[gnu::always_inline]] inline uint32_t waitHalfPeriod(uint32_t edge, const uint32_t half)
{
	/* Schedule the next edge relative to the previous one. */
	edge += half;
	uint32_t now;
	do
	{
		now = esp_cpu_get_cycle_count();
	} while (static_cast<int32_t>(now - edge) < 0);
	/* If we were delayed, restart the schedule here instead of catching up. */
	if (now - edge > (half >> 1U))
		edge = now;
	return edge;
}
}  // namespace

swd_proc_s swd_proc;

void swdptap_init(void)
{
	swd_proc.seq_in = SwdTap::seqIn;
	swd_proc.seq_in_parity = SwdTap::seqInParity;
	swd_proc.seq_out = SwdTap::seqOut;
	swd_proc.seq_out_parity = SwdTap::seqOutParity;
}

IRAM_ATTR void SwdTap::seqOutRaw(uint32_t value, const size_t clockCycles, const uint32_t half)
{
	uint32_t edge = esp_cpu_get_cycle_count();
	for (size_t cycle = 0; cycle < clockCycles; ++cycle)
	{
		/* Set SWDIO according to the LSB of value and drive SWCLK low */
		dedic_gpio_cpu_ll_write_mask(clkMask | dioMask, (value & 1U) ? dioMask : 0U);
		/* Shift value to prepare the next bit for output */
		value >>= 1U;
		/* wait for the first half period */
		edge = waitHalfPeriod(edge, half);
		/* Drive SWCLK high for the second half period */
		dedic_gpio_cpu_ll_write_mask(clkMask, clkMask);
		/* wait for the second half period */
		edge = waitHalfPeriod(edge, half);
	}
	/* Release SWDIO and drive SWCLK low at the end of the sequence */
	dedic_gpio_cpu_ll_write_mask(clkMask, 0U);
}

IRAM_ATTR uint32_t SwdTap::seqInRaw(const size_t clockCycles, const uint32_t half)
{
	uint32_t edge = esp_cpu_get_cycle_count();
	uint32_t value = 0;
	for (size_t cycle = 0; cycle < clockCycles; ++cycle)
	{
		/* Sample SWDIO while SWCLK is low, immediately before the rising edge. */
		edge = waitHalfPeriod(edge, half);
		const uint32_t bit = (dedic_gpio_cpu_ll_read_in() & dioInMask) ? 1U : 0U;
		dedic_gpio_cpu_ll_write_mask(clkMask, clkMask);
		value |= bit << cycle;
		/* Finish the clock with a falling edge before the next bit. */
		edge = waitHalfPeriod(edge, half);
		dedic_gpio_cpu_ll_write_mask(clkMask, 0U);
	}
	return value;
}

IRAM_ATTR void SwdTap::turnaround(const bool drive, const uint32_t half)
{
	if (drive == swdioDriven)
		return;
	swdioDriven = drive;

	/* Release SWDIO before clocking the turnaround cycle. */
	if (!drive)
		GPIO.enable_w1tc = 1U << swdioGpio;
	uint32_t edge = esp_cpu_get_cycle_count();
	edge = waitHalfPeriod(edge, half);
	dedic_gpio_cpu_ll_write_mask(clkMask, clkMask);
	edge = waitHalfPeriod(edge, half);
	dedic_gpio_cpu_ll_write_mask(clkMask, 0U);
	/* Start driving SWDIO only after the turnaround clock is complete. */
	if (drive)
		GPIO.enable_w1ts = 1U << swdioGpio;
}

IRAM_ATTR uint32_t SwdTap::seqIn(const size_t clockCycles)
{
	const uint32_t half = halfPeriod;
	turnaround(false, half);
	return seqInRaw(clockCycles, half);
}

IRAM_ATTR bool SwdTap::seqInParity(uint32_t* const ret, const size_t clockCycles)
{
	const uint32_t half = halfPeriod;
	turnaround(false, half);
	const uint32_t value = seqInRaw(clockCycles, half);
	const uint32_t parity = seqInRaw(1, half);
	turnaround(true, half);
	*ret = value;
	return static_cast<uint32_t>(__builtin_parity(value)) == parity;
}

IRAM_ATTR void SwdTap::seqOut(const uint32_t tmsStates, const size_t clockCycles)
{
	const uint32_t half = halfPeriod;
	turnaround(true, half);
	seqOutRaw(tmsStates, clockCycles, half);
}

IRAM_ATTR void SwdTap::seqOutParity(const uint32_t tmsStates, const size_t clockCycles)
{
	const uint32_t half = halfPeriod;
	turnaround(true, half);
	seqOutRaw(tmsStates, clockCycles, half);
	seqOutRaw(static_cast<uint32_t>(__builtin_parity(tmsStates)), 1, half);
}

void SwdTap::setDriveEnabled(const bool enable)
{
	if (enable)
	{
		/* SWCLK low, SWDIO high (idle line state) */
		dedic_gpio_cpu_ll_write_mask(clkMask | dioMask, dioMask);
		GPIO.enable_w1ts = (1U << swclkGpio) | (1U << swdioGpio);
		swdioDriven = true;
	}
	else
	{
		GPIO.enable_w1tc = (1U << swclkGpio) | (1U << swdioGpio);
		swdioDriven = false;
	}
}

uint32_t SwdTap::calibrate()
{
	const uint32_t start = esp_cpu_get_cycle_count();
	seqOutRaw(0x55555555U, calibrationClocks, 0);
	const uint32_t cycles = esp_cpu_get_cycle_count() - start;
	return static_cast<uint32_t>(static_cast<uint64_t>(calibrationClocks) * cpuFrequency / cycles);
}

void SwdTap::init()
{
	const int gpios[] = {swclkGpio, swdioGpio};
	dedic_gpio_bundle_config_t config = {};
	config.gpio_array = gpios;
	config.array_size = std::size(gpios);
	config.flags.in_en = 1;
	config.flags.out_en = 1;
	/* The bundle is never deleted, its handle isn't needed after init */
	dedic_gpio_bundle_handle_t bundle = nullptr;
	ESP_ERROR_CHECK(dedic_gpio_new_bundle(&config, &bundle));

	uint32_t outOffset = 0;
	uint32_t inOffset = 0;
	ESP_ERROR_CHECK(dedic_gpio_get_out_offset(bundle, &outOffset));
	ESP_ERROR_CHECK(dedic_gpio_get_in_offset(bundle, &inOffset));
	clkMask = 1U << outOffset;
	dioMask = 1U << (outOffset + 1U);
	dioInMask = 1U << (inOffset + 1U);

	/* Output enables come from GPIO_ENABLE_REG, so turnaround() can switch SWDIO */
	gpio_ll_set_output_enable_ctrl(&GPIO, swclkGpio, false, false);
	gpio_ll_set_output_enable_ctrl(&GPIO, swdioGpio, false, false);
	/* Keep a released SWDIO at a defined level */
	gpio_pullup_en(static_cast<gpio_num_t>(swdioGpio));
	setDriveEnabled(false);

	cpuFrequency = esp_rom_get_cpu_ticks_per_us() * 1000000U;
	maxFrequency = calibrate();
	swdptap_init();

	ESP_LOGI(tag, "SWCLK GPIO%d, SWDIO GPIO%d on core %d, max SWCLK %" PRIu32 " Hz", swclkGpio, swdioGpio, esp_cpu_get_core_id(), maxFrequency);
}

void SwdTap::setFrequency(uint32_t frequency)
{
	if (frequency >= maxFrequency)
	{
		halfPeriod = 0;
		return;
	}
	if (frequency < minFrequency)
		frequency = minFrequency;
	/* Round up, so the clock never exceeds the request */
	halfPeriod = (cpuFrequency + 2U * frequency - 1U) / (2U * frequency);
}

uint32_t SwdTap::getFrequency()
{
	if (!halfPeriod)
		return maxFrequency;
	return std::min(cpuFrequency / (2U * halfPeriod), maxFrequency);
}
