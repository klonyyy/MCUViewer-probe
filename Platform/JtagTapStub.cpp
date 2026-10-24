/*
 * SPDX-FileCopyrightText: 2026 MVProbe contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The probe has no JTAG pins. BMD still links its JTAG scan code (monitor jtag_scan,
 * auto_scan), so this backend behaves like an empty chain: TDO always reads 1.
 */

#include <string.h>

extern "C"
{
#include "general.h"
#include "jtagtap.h"
}

namespace
{
void tapReset() {}

bool tapNext([[maybe_unused]] const bool tms, [[maybe_unused]] const bool tdi)
{
	return true;
}

void tapTmsSeq([[maybe_unused]] const uint32_t tmsStates, [[maybe_unused]] const size_t clockCycles) {}

void tapTdiTdoSeq(uint8_t* const dataOut, [[maybe_unused]] const bool finalTms, [[maybe_unused]] const uint8_t* const dataIn, const size_t clockCycles)
{
	if (dataOut)
		memset(dataOut, 0xff, (clockCycles + 7U) / 8U);
}

void tapTdiSeq([[maybe_unused]] const bool finalTms, [[maybe_unused]] const uint8_t* const dataIn, [[maybe_unused]] const size_t clockCycles) {}

void tapCycle([[maybe_unused]] const bool tms, [[maybe_unused]] const bool tdi, [[maybe_unused]] const size_t clockCycles) {}
}  // namespace

jtag_proc_s jtag_proc;

void jtagtap_init(void)
{
	jtag_proc.jtagtap_reset = tapReset;
	jtag_proc.jtagtap_next = tapNext;
	jtag_proc.jtagtap_tms_seq = tapTmsSeq;
	jtag_proc.jtagtap_tdi_tdo_seq = tapTdiTdoSeq;
	jtag_proc.jtagtap_tdi_seq = tapTdiSeq;
	jtag_proc.jtagtap_cycle = tapCycle;
	jtag_proc.tap_idle_cycles = 1;
}
