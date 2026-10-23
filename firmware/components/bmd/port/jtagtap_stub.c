/*
 * SPDX-FileCopyrightText: 2026 MVProbe contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The probe has no JTAG pins. BMD still links its JTAG scan code (monitor jtag_scan,
 * auto_scan), so this backend behaves like an empty chain: TDO always reads 1.
 */

#include "general.h"
#include "jtagtap.h"

jtag_proc_s jtag_proc;

static void jtagtap_reset(void)
{
}

static bool jtagtap_next(const bool tms, const bool tdi)
{
	(void)tms;
	(void)tdi;
	return true;
}

static void jtagtap_tms_seq(const uint32_t tms_states, const size_t clock_cycles)
{
	(void)tms_states;
	(void)clock_cycles;
}

static void jtagtap_tdi_tdo_seq(
	uint8_t *const data_out, const bool final_tms, const uint8_t *const data_in, const size_t clock_cycles)
{
	(void)final_tms;
	(void)data_in;
	if (data_out)
		memset(data_out, 0xff, (clock_cycles + 7U) / 8U);
}

static void jtagtap_tdi_seq(const bool final_tms, const uint8_t *const data_in, const size_t clock_cycles)
{
	(void)final_tms;
	(void)data_in;
	(void)clock_cycles;
}

static void jtagtap_cycle(const bool tms, const bool tdi, const size_t clock_cycles)
{
	(void)tms;
	(void)tdi;
	(void)clock_cycles;
}

void jtagtap_init(void)
{
	jtag_proc.jtagtap_reset = jtagtap_reset;
	jtag_proc.jtagtap_next = jtagtap_next;
	jtag_proc.jtagtap_tms_seq = jtagtap_tms_seq;
	jtag_proc.jtagtap_tdi_tdo_seq = jtagtap_tdi_tdo_seq;
	jtag_proc.jtagtap_tdi_seq = jtagtap_tdi_seq;
	jtag_proc.jtagtap_cycle = jtagtap_cycle;
	jtag_proc.tap_idle_cycles = 1;
}
