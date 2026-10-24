/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The SWD wake-up sequences and the reset follow the ones in src/target/adiv5_swd.c and
 * src/target/cortexm.c of Black Magic Debug, which keeps them static:
 *   Copyright (C) 2011  Black Sphere Technologies Ltd.
 *   Copyright (C) 2022-2024 1BitSquared <info@1bitsquared.com>
 *
 * Functions that call into BMD can raise its exceptions (a single global setjmp chain). They run
 * inside one TRY each and must not return from inside it, so every TRY body calls a helper.
 */

#include "TargetLink.hpp"

#include "esp_log.h"
#include "sdkconfig.h"

extern "C"
{
#include "adi.h"
#include "adiv5.h"
#include "cortexm.h"
#include "exception.h"
#include "general.h"
#include "platform.h"
}

namespace
{
constexpr const char* tag = "target_link";

constexpr uint32_t powerUpTimeoutMs = 250;
constexpr uint32_t resetTimeoutMs = 1000;
constexpr uint8_t faultSticky = 8;
constexpr uint8_t faultOther = 255;

constexpr uint32_t abortAll = ADIV5_DP_ABORT_ORUNERRCLR | ADIV5_DP_ABORT_WDERRCLR | ADIV5_DP_ABORT_STKERRCLR | ADIV5_DP_ABORT_STKCMPCLR;
constexpr uint32_t powerUpRequest = ADIV5_DP_CTRLSTAT_CSYSPWRUPREQ | ADIV5_DP_CTRLSTAT_CDBGPWRUPREQ;
constexpr uint32_t powerUpAck = ADIV5_DP_CTRLSTAT_CSYSPWRUPACK | ADIV5_DP_CTRLSTAT_CDBGPWRUPACK;
/* DLPIDR, DP bank 3 on DPv2: its TINSTANCE goes into TARGETSEL */
constexpr uint16_t dpDlpidr = ADIV5_DP_REG(0x4U);

/* The link is static, so the AP never frees the DP (refcnt) */
adiv5_debug_port_s dp;
adiv5_access_port_s ap;
bool connected = false;
TargetLink::Error lastError = TargetLink::Error::none;
uint8_t lastFault = 0;
uint32_t errorCount = 0;

/* SWDIO high for 60 clocks, then 4 idle clocks if requested */
void lineReset(const bool idleCycles)
{
	swd_proc.seq_out(0xffffffffU, 32U);
	swd_proc.seq_out(0x0fffffffU, idleCycles ? 32U : 28U);
}

/* ADIv5.2 §B5.3.4: JTAG to dormant, selection alert and SWD activation code */
void dormantToSwd()
{
	lineReset(false);
	swd_proc.seq_out(ADIV5_JTAG_TO_DORMANT_SEQUENCE0, 5U);
	swd_proc.seq_out(ADIV5_JTAG_TO_DORMANT_SEQUENCE1, 31U);
	swd_proc.seq_out(ADIV5_JTAG_TO_DORMANT_SEQUENCE2, 8U);
	swd_proc.seq_out(ADIV5_SELECTION_ALERT_SEQUENCE_0, 32U);
	swd_proc.seq_out(ADIV5_SELECTION_ALERT_SEQUENCE_1, 32U);
	swd_proc.seq_out(ADIV5_SELECTION_ALERT_SEQUENCE_2, 32U);
	swd_proc.seq_out(ADIV5_SELECTION_ALERT_SEQUENCE_3, 32U);
	/* 4 idle clocks with SWDIO low, then the 8-bit activation code */
	swd_proc.seq_out(ADIV5_ACTIVATION_CODE_ARM_SWD_DP << 4U, 12U);
	/* The target is in the protocol error state after selecting SWD */
	lineReset(true);
}

/* Deprecated JTAG-to-SWD select sequence, for debug ports without a dormant state */
void jtagToSwd()
{
	lineReset(false);
	swd_proc.seq_out(ADIV5_JTAG_TO_SWD_SELECT_SEQUENCE, 16U);
	lineReset(true);
}

void setupDebugPort()
{
	dp = {};
	dp.refcnt = 1;
	dp.write_no_check = adiv5_swd_write_no_check;
	dp.read_no_check = adiv5_swd_read_no_check;
	dp.dp_read = adiv5_swd_read;
	dp.low_access = adiv5_swd_raw_access;
	dp.error = adiv5_swd_clear_error;
	dp.abort = adiv5_swd_abort;
	dp.ap_read = adiv5_ap_reg_read;
	dp.ap_write = adiv5_ap_reg_write;
	dp.mem_read = adiv5_mem_read_bytes;
	dp.mem_write = adiv5_mem_write_bytes;
}

bool fail(const TargetLink::Error error, const uint8_t fault = 0)
{
	lastError = error;
	lastFault = fault;
	if (error == TargetLink::Error::swd)
		++errorCount;
	return false;
}

/*
 * After an access: a fault leaves the DP refusing AP accesses until it is cleared, and a sticky
 * error can remain from a posted access. Both fail the operation; adiv5_dp_error() clears them.
 */
bool checkAccess()
{
	const uint8_t fault = dp.fault;
	const uint32_t sticky = adiv5_dp_error(&dp);
	if (fault)
		return fail(TargetLink::Error::swd, fault);
	if (sticky)
		return fail(TargetLink::Error::swd, faultSticky);
	return true;
}

/* Called from CATCH: record the exception, then clear the DP state, which may raise again */
bool failWithException(const char* const message)
{
	ESP_LOGW(tag, "SWD exception: %s", message ? message : "");
	const uint8_t fault = dp.fault ? dp.fault : faultOther;
	TRY(EXCEPTION_ALL)
	{
		adiv5_dp_error(&dp);
	}
	CATCH()
	{
		default:
			dp.fault = 0;
	}
	return fail(TargetLink::Error::swd, fault);
}

bool lightConnect(const uint8_t apSel, TargetLink::ConnectInfo& info)
{
	dormantToSwd();
	uint32_t dpidr = adiv5_dp_read_dpidr(&dp);
	if (!dpidr)
	{
		jtagToSwd();
		dp.fault = 0;
		dpidr = adiv5_dp_read_dpidr(&dp);
	}
	if (!dpidr)
		return fail(TargetLink::Error::noTarget);

	/* On DPv2 the register at 0x0 depends on SELECT.DPBANKSEL: read DPIDR again from bank 0, as adiv5_dp_init() does */
	dp.version = (dpidr & ADIV5_DP_DPIDR_VERSION_MASK) >> ADIV5_DP_DPIDR_VERSION_OFFSET;
	adiv5_dp_write(&dp, ADIV5_DP_SELECT, ADIV5_DP_BANK0);
	dpidr = adiv5_dp_read_dpidr(&dp);
	dp.version = (dpidr & ADIV5_DP_DPIDR_VERSION_MASK) >> ADIV5_DP_DPIDR_VERSION_OFFSET;
	if (!(dpidr & 1U) || dp.version == 0U)
		return fail(TargetLink::Error::noTarget);
	if (dp.version >= 3U)
		return fail(TargetLink::Error::unsupportedDp);
	dp.designer_code = adi_decode_designer((dpidr & ADIV5_DP_DPIDR_DESIGNER_MASK) >> ADIV5_DP_DPIDR_DESIGNER_OFFSET);
	dp.partno = (dpidr & ADIV5_DP_DPIDR_PARTNO_MASK) >> ADIV5_DP_DPIDR_PARTNO_OFFSET;
	dp.quirks = (dpidr >> ADIV5_DP_DPIDR_MINDP_OFFSET) & ADIV5_DP_QUIRK_MINDP;

	uint32_t targetid = 0;
	if (dp.version >= 2U)
	{
		adiv5_dp_write(&dp, ADIV5_DP_SELECT, ADIV5_DP_BANK2);
		targetid = adiv5_dp_read(&dp, ADIV5_DP_TARGETID);
		adiv5_dp_write(&dp, ADIV5_DP_SELECT, ADIV5_DP_BANK3);
		const uint32_t dlpidr = adiv5_dp_read(&dp, dpDlpidr);
		adiv5_dp_write(&dp, ADIV5_DP_SELECT, ADIV5_DP_BANK0);

		dp.target_designer_code = adi_decode_designer((targetid & ADIV5_DP_TARGETID_TDESIGNER_MASK) >> ADIV5_DP_TARGETID_TDESIGNER_OFFSET);
		dp.target_partno = (targetid & ADIV5_DP_TARGETID_TPARTNO_MASK) >> ADIV5_DP_TARGETID_TPARTNO_OFFSET;
		/* adiv5_swd_clear_error() selects the target with this after a line reset */
		dp.dev_index = (dlpidr & ADIV5_DP_TARGETSEL_TINSTANCE_MASK) >> ADIV5_DP_TARGETSEL_TINSTANCE_OFFSET;
		dp.targetsel = (dlpidr & ADIV5_DP_TARGETSEL_TINSTANCE_MASK) | (targetid & (ADIV5_DP_TARGETID_TDESIGNER_MASK | ADIV5_DP_TARGETID_TPARTNO_MASK)) | 1U;
	}

	/* Clear sticky errors, then request debug and system power without power cycling the debug domain first */
	adiv5_dp_abort(&dp, abortAll);
	adiv5_dp_write(&dp, ADIV5_DP_CTRLSTAT, powerUpRequest);
	platform_timeout_s timeout;
	platform_timeout_set(&timeout, powerUpTimeoutMs);
	while ((adiv5_dp_read(&dp, ADIV5_DP_CTRLSTAT) & powerUpAck) != powerUpAck)
	{
		if (platform_timeout_is_expired(&timeout))
			return fail(TargetLink::Error::noPowerUp);
		platform_delay(1);
	}

	/* adi_configure_ap() only reads AP registers and computes the CSW to use */
	ap = {};
	ap.refcnt = 1;
	ap.dp = &dp;
	ap.apsel = apSel;
	if (!adi_configure_ap(&ap) || ADIV5_AP_IDR_CLASS(ap.idr) != ADIV5_AP_IDR_CLASS_MEM || dp.fault)
		return fail(TargetLink::Error::noMemAp);

	info.dpidr = dpidr;
	info.targetid = targetid;
	info.apIdr = ap.idr;
	info.frequency = platform_max_frequency_get();
	return true;
}

bool readMemory(const uint32_t address, void* const data, const size_t length)
{
	adiv5_mem_read(&ap, data, address, length);
	return checkAccess();
}

bool writeMemory(const uint32_t address, const void* const data, const size_t length)
{
	adiv5_mem_write(&ap, address, data, length);
	return checkAccess();
}

/*
 * Like adiv5_mem_read_bytes(), but with the access width given instead of derived from address and length, so a
 * block of 16-bit registers stays 16-bit accesses. No TAR reload: blocks do not cross the 1 KiB auto-increment
 * boundary. A fault leaves dp.fault set, and the AP reads after it return 0 without bus traffic.
 */
bool readPlanBlocks(const TargetLink::Block* const blocks, const size_t count, uint8_t* data)
{
	for (size_t i = 0; i < count; ++i)
	{
		const TargetLink::Block& block = blocks[i];
		const align_e align = block.accessSize == 4 ? ALIGN_32BIT : (block.accessSize == 2 ? ALIGN_16BIT : ALIGN_8BIT);
		adi_ap_mem_access_setup(&ap, block.address, align);
		for (uint32_t access = 0; access < block.count; ++access)
		{
			const uint32_t value = adiv5_dp_read(&dp, ADIV5_AP_DRW);
			data = static_cast<uint8_t*>(adiv5_unpack_data(data, block.address + access * block.accessSize, value, align));
		}
	}

	if (dp.fault)
	{
		const uint8_t fault = dp.fault;
		adiv5_dp_error(&dp);
		return fail(TargetLink::Error::swd, fault);
	}
	return true;
}

/* AIRCR.SYSRESETREQ: the access itself can fail as the reset takes effect, which is expected */
void requestSystemReset()
{
	const uint32_t aircr = CORTEXM_AIRCR_VECTKEY | CORTEXM_AIRCR_SYSRESETREQ;
	TRY(EXCEPTION_ALL)
	{
		adiv5_mem_write(&ap, CORTEXM_AIRCR, &aircr, sizeof(aircr));
		adiv5_dp_error(&dp);
	}
	CATCH()
	{
		default:
			dp.fault = 0;
	}
}

bool resetTarget()
{
	uint32_t dhcsr = 0;
	/* Reading DHCSR clears S_RESET_ST, so the next read shows whether a reset happened since */
	adiv5_mem_read(&ap, &dhcsr, CORTEXM_DHCSR, sizeof(dhcsr));
	adiv5_dp_error(&dp);

#if CONFIG_NETWORK_PROBE_NRST_GPIO >= 0
	platform_nrst_set_val(true);
	platform_delay(1);
	platform_nrst_set_val(false);
	platform_delay(10);
	adiv5_dp_error(&dp);
#endif

	adiv5_mem_read(&ap, &dhcsr, CORTEXM_DHCSR, sizeof(dhcsr));
	if (dp.fault || !(dhcsr & CORTEXM_DHCSR_S_RESET_ST))
	{
		/* nRST is not configured, not wired, or does not reset this target */
		adiv5_dp_error(&dp);
		requestSystemReset();
	}

	/* S_RESET_ST reads 0 again once the core has left reset */
	platform_timeout_s timeout;
	platform_timeout_set(&timeout, resetTimeoutMs);
	while (true)
	{
		adiv5_dp_error(&dp);
		adiv5_mem_read(&ap, &dhcsr, CORTEXM_DHCSR, sizeof(dhcsr));
		if (!dp.fault && !(dhcsr & CORTEXM_DHCSR_S_RESET_ST))
			break;
		if (platform_timeout_is_expired(&timeout))
		{
			adiv5_dp_error(&dp);
			return fail(TargetLink::Error::swd, faultOther);
		}
		platform_delay(1);
	}

	/* Let clocks such as the STM32 HSI start up, as BMD does */
	platform_delay(10);
	adiv5_dp_error(&dp);
	return true;
}
}  // namespace

bool TargetLink::connect(const uint32_t frequency, const uint8_t apSel, ConnectInfo& info)
{
	connected = false;
	lastError = Error::none;
	lastFault = 0;

	platform_max_frequency_set(frequency);
	setupDebugPort();
	swdptap_init();
	platform_target_clk_output_enable(true);

	volatile bool ok = false;
	TRY(EXCEPTION_ALL)
	{
		ok = lightConnect(apSel, info);
	}
	CATCH()
	{
		default:
			ok = failWithException(exception_frame.msg);
	}

	if (!ok)
	{
		ESP_LOGW(tag, "light connect failed: error %u, fault %u", static_cast<unsigned>(lastError), lastFault);
		platform_target_clk_output_enable(false);
		return false;
	}

	ESP_LOGI(tag, "connected: DPIDR 0x%08" PRIx32 ", AP%u IDR 0x%08" PRIx32 ", SWCLK %" PRIu32 " Hz", info.dpidr, apSel, info.apIdr, info.frequency);
	connected = true;
	return true;
}

void TargetLink::disconnect()
{
	if (connected)
	{
		TRY(EXCEPTION_ALL)
		{
			adiv5_dp_write(&dp, ADIV5_DP_CTRLSTAT, 0U);
		}
		CATCH()
		{
			default:
				dp.fault = 0;
		}
		ESP_LOGI(tag, "disconnected");
	}
	platform_target_clk_output_enable(false);
	connected = false;
}

bool TargetLink::isConnected()
{
	return connected;
}

bool TargetLink::read(const uint32_t address, void* const data, const size_t length)
{
	if (!connected)
		return fail(Error::notConnected);

	volatile bool ok = false;
	TRY(EXCEPTION_ALL)
	{
		ok = readMemory(address, data, length);
	}
	CATCH()
	{
		default:
			ok = failWithException(exception_frame.msg);
	}
	return ok;
}

bool TargetLink::write(const uint32_t address, const void* const data, const size_t length)
{
	if (!connected)
		return fail(Error::notConnected);

	volatile bool ok = false;
	TRY(EXCEPTION_ALL)
	{
		ok = writeMemory(address, data, length);
	}
	CATCH()
	{
		default:
			ok = failWithException(exception_frame.msg);
	}
	return ok;
}

bool TargetLink::readBlocks(const Block* const blocks, const size_t count, uint8_t* const data)
{
	if (!connected)
		return fail(Error::notConnected);

	volatile bool ok = false;
	TRY(EXCEPTION_ALL)
	{
		ok = readPlanBlocks(blocks, count, data);
	}
	CATCH()
	{
		default:
			ok = failWithException(exception_frame.msg);
	}
	return ok;
}

bool TargetLink::reset()
{
	if (!connected)
		return fail(Error::notConnected);

	volatile bool ok = false;
	TRY(EXCEPTION_ALL)
	{
		ok = resetTarget();
	}
	CATCH()
	{
		default:
			ok = failWithException(exception_frame.msg);
	}
	ESP_LOGI(tag, "target reset %s", ok ? "done" : "failed");
	return ok;
}

TargetLink::Error TargetLink::getError()
{
	return lastError;
}

uint8_t TargetLink::getFault()
{
	return lastFault;
}

uint32_t TargetLink::getErrorCount()
{
	return errorCount;
}
