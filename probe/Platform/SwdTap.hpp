/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
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
	static constexpr uint32_t minFrequency = 1000; /**< setFrequency() clamps lower requests to this. */

	/**
	 * @brief Takes SWCLK and SWDIO, releases both lines, measures the fastest SWCLK the loop can produce and points
	 * swd_proc at the tap.
	 */
	static void init();

	/**
	 * @brief Sets the SWCLK frequency, rounded so the clock never exceeds the request. A request at or above the maximum
	 * gets the maximum.
	 */
	static void setFrequency(uint32_t frequency);

	/**
	 * @brief Gets the SWCLK frequency derived from the half period setting.
	 */
	static uint32_t getFrequency();

	/**
	 * @brief Gets the fastest SWCLK the tap times, from the speed of the loop measured at init.
	 */
	static uint32_t getMaxFrequency() { return maxFrequency; }

	/**
	 * @brief Drives (true) or releases (false) SWCLK and SWDIO.
	 */
	static void setDriveEnabled(bool enable);

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
	 */
	static void turnaround(bool drive, uint32_t half);

	/**
	 * @brief Runs the output loop at full speed with both pins released, a few times.
	 * @return The fastest SWCLK the loop can produce, in Hz.
	 */
	static uint32_t calibrate();

	/* Dedicated GPIO channel masks */
	static inline uint32_t clkMask = 0;
	static inline uint32_t dioMask = 0;
	static inline uint32_t dioInMask = 0;

	static inline uint32_t cpuFrequency = 0;
	static inline uint32_t halfPeriod = 0;	  /**< CPU cycles per SWCLK half period, 0 until setFrequency(). */
	static inline uint32_t minHalfPeriod = 1; /**< The shortest half period the loop keeps, from the measurement at init. */
	static inline uint32_t maxFrequency = 0;  /**< SWCLK at minHalfPeriod. */
	static inline bool swdioDriven = false;
};

#endif /* SWDTAP_HPP */
