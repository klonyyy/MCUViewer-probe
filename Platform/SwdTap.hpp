/*
 * SPDX-FileCopyrightText: 2026 MVProbe contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SWDTAP_HPP
#define SWDTAP_HPP

#include <stddef.h>
#include <stdint.h>

/**
 * @brief SWD bit-bang tap on dedicated GPIO (CPU GPIO instructions), the backend of BMD's swd_proc.
 *
 * The tap drives SWCLK and SWDIO through a dedicated GPIO bundle, which only the CPU core that created it can drive.
 * init() must therefore be called from the task that will own SWD, and that task must be pinned to a core.
 */
class SwdTap
{
   public:
	/**
	 * @brief Counters of the tap primitives, to measure the SWCLK frequency they actually produce.
	 */
	struct Stats
	{
		uint64_t clocks; /**< SWCLK cycles emitted. */
		uint64_t cycles; /**< CPU cycles spent inside the tap primitives. */
	};

	/**
	 * @brief Takes SWCLK and SWDIO, releases both lines, measures the fastest SWCLK the loop can produce and points
	 * swd_proc at the tap.
	 */
	static void init();

	/**
	 * @brief Sets the SWCLK frequency, rounded so the clock never exceeds the request. At or above the measured maximum
	 * the loop runs as fast as it can.
	 */
	static void setFrequency(uint32_t frequency);

	/**
	 * @brief Gets the nominal SWCLK frequency derived from the half period setting.
	 */
	static uint32_t getFrequency();

	/**
	 * @brief Gets the CPU cycles per SWCLK half period, 0 when the loop runs as fast as it can.
	 */
	static uint32_t getHalfPeriodCycles() { return halfPeriod; }

	static uint32_t getCpuFrequency() { return cpuFrequency; }

	/**
	 * @brief Gets the free-running CPU cycle counter of the calling core.
	 */
	static uint32_t getCycleCount();

	/**
	 * @brief Drives (true) or releases (false) SWCLK and SWDIO.
	 */
	static void setDriveEnabled(bool enable);

	static Stats getStats() { return stats; }
	static void resetStats() { stats = {}; }

	/* swd_proc primitives, see BMD's swd.h. Each one ends with a falling SWCLK edge. */
	static uint32_t seqIn(size_t clockCycles);
	static bool seqInParity(uint32_t* ret, size_t clockCycles);
	static void seqOut(uint32_t tmsStates, size_t clockCycles);
	static void seqOutParity(uint32_t tmsStates, size_t clockCycles);

   private:
	static void seqOutRaw(uint32_t value, size_t clockCycles, uint32_t half);
	static uint32_t seqInRaw(size_t clockCycles, uint32_t half);

	/**
	 * @brief Turns SWDIO around if its direction differs from drive.
	 * @return Clock cycles spent, 0 or 1.
	 */
	static size_t turnaround(bool drive, uint32_t half);

	static void addStats(uint32_t start, size_t clocks);

	/**
	 * @brief Runs the output loop at full speed with both pins released.
	 * @return The fastest SWCLK the loop can produce, in Hz.
	 */
	static uint32_t calibrate();

	/* Dedicated GPIO channel masks */
	static inline uint32_t clkMask = 0;
	static inline uint32_t dioMask = 0;
	static inline uint32_t dioInMask = 0;

	static inline uint32_t cpuFrequency = 0;
	static inline uint32_t halfPeriod = 0;	 /**< CPU cycles per SWCLK half period, 0 = as fast as the loop runs. */
	static inline uint32_t maxFrequency = 0; /**< Fastest SWCLK the loop can produce, measured at init. */
	static inline bool swdioDriven = false;
	static inline Stats stats{};
};

#endif /* SWDTAP_HPP */
