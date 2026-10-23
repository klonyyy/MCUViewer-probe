/*
 * SPDX-FileCopyrightText: 2026 MVProbe contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Probe-specific GDB monitor commands.
 */

#include "general.h"
#include "platform.h"
#include "bmd_port.h"
#include "exception.h"
#include "gdb_packet.h"
#include "target.h"
#include "target_internal.h"
#include "adiv5.h"
#include "cortexm.h"

#define SWD_INFO_ITERATIONS 1000U

static bool cmd_swd_info(target_s *target, int argc, const char **argv);

const command_s platform_cmd_list[] = {
	{"swd_info", cmd_swd_info, "Report the SWD clock and measure SWD transaction timing"},
	{NULL, NULL, NULL},
};

/* Print a duration given in CPU cycles as microseconds with two decimals */
static void print_us(const char *const label, const uint64_t cycles, const uint32_t count)
{
	const uint64_t cpu_mhz = swd_tap_cpu_frequency() / 1000000U;
	const uint64_t ns = cycles * 1000U / cpu_mhz / count;
	gdb_outf("%s: %" PRIu32 ".%02" PRIu32 " us\n", label, (uint32_t)(ns / 1000U), (uint32_t)(ns % 1000U / 10U));
}

static void print_measured_clock(void)
{
	swd_tap_stats_s stats;
	swd_tap_stats_get(&stats);
	if (!stats.cycles)
		return;
	const uint64_t hz = stats.clocks * swd_tap_cpu_frequency() / stats.cycles;
	gdb_outf("SWD clock measured inside tap primitives: %" PRIu32 " Hz (%" PRIu32 " clocks)\n", (uint32_t)hz,
		(uint32_t)stats.clocks);
}

static bool swd_info_measure(target_s *const target)
{
	adiv5_access_port_s *const ap = cortex_ap(target);
	adiv5_debug_port_s *const dp = ap->dp;
	volatile uint32_t dp_cycles = 0;
	volatile uint32_t mem_cycles = 0;

	/* swd_scan releases the SWD lines when it finishes; only attach drives them again */
	platform_target_clk_output_enable(true);
	TRY (EXCEPTION_ALL) {
		/* One SWD transaction each: raw DPIDR read */
		swd_tap_stats_reset();
		uint32_t start = swd_tap_cycle_count();
		for (size_t i = 0; i < SWD_INFO_ITERATIONS; ++i)
			adiv5_dp_low_access(dp, ADIV5_LOW_READ, ADIV5_DP_DPIDR, 0U);
		dp_cycles = swd_tap_cycle_count() - start;
		print_measured_clock();

		/* BMD's generic MEM-AP path: CSW, TAR, DRW and RDBUFF for a single word */
		uint32_t value = 0;
		start = swd_tap_cycle_count();
		for (size_t i = 0; i < SWD_INFO_ITERATIONS; ++i)
			adiv5_mem_read(ap, &value, CORTEXM_CPUID, sizeof(value));
		mem_cycles = swd_tap_cycle_count() - start;
	}
	CATCH () {
	default:
		gdb_outf("SWD error during measurement: %s\n", exception_frame.msg);
		if (!target->attached)
			platform_target_clk_output_enable(false);
		return false;
	}

	const uint32_t errors = adiv5_dp_error(dp);
	if (!target->attached)
		platform_target_clk_output_enable(false);
	print_us("DPIDR read (1 transaction)", dp_cycles, SWD_INFO_ITERATIONS);
	print_us("32-bit memory read via BMD (5 transactions)", mem_cycles, SWD_INFO_ITERATIONS);
	if (errors)
		gdb_outf("Sticky errors after measurement: 0x%08" PRIx32 "\n", errors);
	return true;
}

static bool cmd_swd_info(target_s *target, int argc, const char **argv)
{
	(void)argc;
	(void)argv;

	gdb_outf("SWD clock nominal: %" PRIu32 " Hz (half period %" PRIu32 " cycles, CPU %" PRIu32 " MHz)\n",
		swd_tap_get_frequency(), swd_tap_half_period_cycles(), swd_tap_cpu_frequency() / 1000000U);
	print_measured_clock();

	if (!target)
		target = target_list;
	if (!target || !target_is_cortexm(target)) {
		gdb_out("No Cortex-M target: run swd_scan first to measure transaction timing\n");
		return true;
	}
	return swd_info_measure(target);
}
