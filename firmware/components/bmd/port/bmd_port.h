/*
 * SPDX-FileCopyrightText: 2026 MVProbe contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Interface of the ESP32-S3 port beyond what Black Magic Debug itself requires.
 */

#ifndef BMD_PORT_H
#define BMD_PORT_H

#include <stdbool.h>
#include <stdint.h>

/*
 * SWD bit-bang tap (swdptap_esp.c).
 *
 * The tap uses a dedicated GPIO bundle, which only the CPU core that created it can drive.
 * swd_tap_hw_init() must therefore be called from the task that will own SWD, and that
 * task must be pinned to a core.
 */
void swd_tap_hw_init(void);
void swd_tap_set_frequency(uint32_t frequency);
/* Nominal SWCLK frequency derived from the half-period setting */
uint32_t swd_tap_get_frequency(void);
uint32_t swd_tap_half_period_cycles(void);
uint32_t swd_tap_cpu_frequency(void);
/* Free-running CPU cycle counter of the calling core */
uint32_t swd_tap_cycle_count(void);
/* Drive (true) or release (false) SWCLK and SWDIO */
void swd_tap_drive_enable(bool enable);

typedef struct swd_tap_stats {
	uint64_t clocks; /* SWCLK cycles emitted */
	uint64_t cycles; /* CPU cycles spent inside the tap primitives */
} swd_tap_stats_s;

void swd_tap_stats_get(swd_tap_stats_s *stats);
void swd_tap_stats_reset(void);

#endif /* BMD_PORT_H */
