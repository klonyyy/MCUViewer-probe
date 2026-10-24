/*
 * SPDX-FileCopyrightText: 2026 MVProbe contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Probe-specific GDB monitor commands.
 */

#include "SwdTap.hpp"

extern "C"
{
#include "adiv5.h"
#include "cortexm.h"
#include "exception.h"
#include "gdb_packet.h"
#include "general.h"
#include "platform.h"
#include "target.h"
#include "target_internal.h"
}

namespace
{
constexpr uint32_t swdInfoIterations = 1000;

/* Print a duration given in CPU cycles as microseconds with two decimals */
void printUs(const char* const label, const uint64_t cycles, const uint32_t count)
{
	const uint64_t cpuMhz = SwdTap::getCpuFrequency() / 1000000U;
	const uint64_t ns = cycles * 1000U / cpuMhz / count;
	gdb_outf("%s: %" PRIu32 ".%02" PRIu32 " us\n", label, static_cast<uint32_t>(ns / 1000U), static_cast<uint32_t>(ns % 1000U / 10U));
}

void printMeasuredClock()
{
	const SwdTap::Stats stats = SwdTap::getStats();
	if (!stats.cycles)
		return;
	const uint64_t hz = stats.clocks * SwdTap::getCpuFrequency() / stats.cycles;
	gdb_outf("SWD clock measured inside tap primitives: %" PRIu32 " Hz (%" PRIu32 " clocks)\n", static_cast<uint32_t>(hz), static_cast<uint32_t>(stats.clocks));
}

bool measureSwdTiming(target_s* const target)
{
	adiv5_access_port_s* const ap = cortex_ap(target);
	adiv5_debug_port_s* const dp = ap->dp;
	volatile uint32_t dpCycles = 0;
	volatile uint32_t memCycles = 0;

	/* swd_scan releases the SWD lines when it finishes; only attach drives them again */
	platform_target_clk_output_enable(true);
	TRY(EXCEPTION_ALL)
	{
		/* One SWD transaction each: raw DPIDR read */
		SwdTap::resetStats();
		uint32_t start = SwdTap::getCycleCount();
		for (uint32_t i = 0; i < swdInfoIterations; ++i)
			adiv5_dp_low_access(dp, ADIV5_LOW_READ, ADIV5_DP_DPIDR, 0U);
		dpCycles = SwdTap::getCycleCount() - start;
		printMeasuredClock();

		/* BMD's generic MEM-AP path: CSW, TAR, DRW and RDBUFF for a single word */
		uint32_t value = 0;
		start = SwdTap::getCycleCount();
		for (uint32_t i = 0; i < swdInfoIterations; ++i)
			adiv5_mem_read(ap, &value, CORTEXM_CPUID, sizeof(value));
		memCycles = SwdTap::getCycleCount() - start;
	}
	CATCH()
	{
		default:
			gdb_outf("SWD error during measurement: %s\n", exception_frame.msg);
			if (!target->attached)
				platform_target_clk_output_enable(false);
			return false;
	}

	const uint32_t errors = adiv5_dp_error(dp);
	if (!target->attached)
		platform_target_clk_output_enable(false);
	printUs("DPIDR read (1 transaction)", dpCycles, swdInfoIterations);
	printUs("32-bit memory read via BMD (5 transactions)", memCycles, swdInfoIterations);
	if (errors)
		gdb_outf("Sticky errors after measurement: 0x%08" PRIx32 "\n", errors);
	return true;
}

bool cmdSwdInfo(target_s* target, [[maybe_unused]] int argc, [[maybe_unused]] const char** argv)
{
	gdb_outf("SWD clock nominal: %" PRIu32 " Hz (half period %" PRIu32 " cycles, CPU %" PRIu32 " MHz)\n", SwdTap::getFrequency(), SwdTap::getHalfPeriodCycles(), SwdTap::getCpuFrequency() / 1000000U);
	printMeasuredClock();

	if (!target)
		target = target_list;
	if (!target || !target_is_cortexm(target))
	{
		gdb_out("No Cortex-M target: run swd_scan first to measure transaction timing\n");
		return true;
	}
	return measureSwdTiming(target);
}
}  // namespace

const command_s platform_cmd_list[] = {
	{"swd_info", cmdSwdInfo, "Report the SWD clock and measure SWD transaction timing"},
	{nullptr, nullptr, nullptr},
};
