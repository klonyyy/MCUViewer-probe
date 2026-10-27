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

#include <algorithm>

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
constexpr uint32_t waitTimeoutMs = 250; /**< How long an access may answer WAIT, as in BMD. */
constexpr uint8_t faultParity = 1;
constexpr uint8_t faultSticky = 8;
constexpr uint8_t faultOther = 255;
constexpr uint64_t autoIncrementPage = 0x400; /**< TAR auto-increment only carries within 1 KiB. */

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

/*
 * One SWD transaction without the retries of adiv5_swd_raw_access(): that one answers a FAULT by clearing the sticky
 * error and repeating the access, which in a pipelined read hides the failed access before it. WAIT is retried for
 * waitTimeoutMs. No idle clocks follow, so the next transaction starts at once. A failure sets dp.fault to the ACK, or
 * to faultParity.
 */
bool transfer(const uint8_t request, const bool read, uint32_t& value)
{
	platform_timeout_s timeout;
	bool waiting = false;
	while (true)
	{
		swd_proc.seq_out(request, 8U);
		const uint8_t ack = swd_proc.seq_in(3U);
		if (ack == SWD_ACK_OK)
		{
			if (!read)
			{
				swd_proc.seq_out_parity(value, 32U);
				return true;
			}
			if (swd_proc.seq_in_parity(&value, 32U))
				return true;
			dp.fault = faultParity;
			return false;
		}
		if (ack == SWD_ACK_WAIT)
		{
			if (!waiting)
			{
				platform_timeout_set(&timeout, waitTimeoutMs);
				waiting = true;
				continue;
			}
			if (!platform_timeout_is_expired(&timeout))
				continue;
			/* The AP is stuck in the access */
			adiv5_dp_abort(&dp, ADIV5_DP_ABORT_DAPABORT);
		}
		dp.fault = ack;
		return false;
	}
}

bool writeRegister(const uint16_t address, uint32_t value)
{
	return transfer(make_packet_request(ADIV5_LOW_WRITE, address), false, value);
}

/* Selects bank 0 of the AP, then programs CSW for sequential accesses of the given width and TAR, as
   adi_ap_mem_access_setup() does */
bool setupAccess(const uint32_t address, const align_e align)
{
	uint32_t csw = ap.csw | ADIV5_AP_CSW_ADDRINC_SINGLE;
	if (align == ALIGN_8BIT)
		csw |= ADIV5_AP_CSW_SIZE_BYTE;
	else if (align == ALIGN_16BIT)
		csw |= ADIV5_AP_CSW_SIZE_HALFWORD;
	else
		csw |= ADIV5_AP_CSW_SIZE_WORD;

	if (!writeRegister(ADIV5_DP_SELECT, static_cast<uint32_t>(ap.apsel) << 24U) || !writeRegister(ADIV5_AP_CSW, csw))
		return false;
	if ((ap.flags & ADIV5_AP_FLAGS_64BIT) && !writeRegister(ADIV5_AP_TAR_HIGH, 0U))
		return false;
	return writeRegister(ADIV5_AP_TAR_LOW, address);
}

/*
 * Reads count accesses through DRW with the AP set up for them, pipelined: an AP read returns the result of the one
 * before it, so the reads follow each other and RDBUFF returns the last one. The accesses must stay inside one 1 KiB
 * auto-increment page. Returns the end of the data, or nullptr when a transaction failed.
 */
uint8_t* readPipelined(uint32_t address, const uint32_t count, const align_e align, uint8_t* data)
{
	const uint8_t drwRead = make_packet_request(ADIV5_LOW_READ, ADIV5_AP_DRW);
	const uint32_t stride = 1U << align;
	uint32_t value = 0;

	if (!transfer(drwRead, true, value))
		return nullptr;
	for (uint32_t access = 1; access < count; ++access, address += stride)
	{
		if (!transfer(drwRead, true, value))
			return nullptr;
		data = static_cast<uint8_t*>(adiv5_unpack_data(data, address, value, align));
	}
	if (!transfer(make_packet_request(ADIV5_LOW_READ, ADIV5_DP_RDBUFF), true, value))
		return nullptr;
	data = static_cast<uint8_t*>(adiv5_unpack_data(data, address, value, align));

	/* Idle clocks complete the last transaction */
	swd_proc.seq_out(0U, 8U);
	return data;
}

bool readMemory(const uint32_t address, void* const data, const size_t length)
{
	if (length == 0)
		return true;

	const align_e align = MIN_ALIGN(address, length);
	const uint64_t end = static_cast<uint64_t>(address) + length;
	uint8_t* out = static_cast<uint8_t*>(data);

	if (setupAccess(address, align))
	{
		/* TAR is written again at every page, as auto-increment does not carry past it */
		for (uint64_t page = address; page < end && out;)
		{
			const uint64_t pageEnd = std::min(end, (page | (autoIncrementPage - 1U)) + 1U);
			if (page != address && !writeRegister(ADIV5_AP_TAR_LOW, static_cast<uint32_t>(page)))
				break;
			out = readPipelined(static_cast<uint32_t>(page), static_cast<uint32_t>((pageEnd - page) >> align), align, out);
			page = pageEnd;
		}
	}
	return checkAccess();
}

bool writeMemory(const uint32_t address, const void* const data, const size_t length)
{
	adiv5_mem_write(&ap, address, data, length);
	return checkAccess();
}

/*
 * With the access width given instead of derived from address and length, so a block of 16-bit registers stays
 * 16-bit accesses. No TAR reload: blocks do not cross the 1 KiB auto-increment boundary. CSW is only written again when
 * the width changes, so most blocks only write TAR. Stops at the first failed transaction.
 */
bool readPlanBlocks(const TargetLink::Block* const blocks, const size_t count, uint8_t* data)
{
	align_e setUpAlign = ALIGN_32BIT;
	for (size_t i = 0; i < count && data; ++i)
	{
		const TargetLink::Block& block = blocks[i];
		const align_e align = block.accessSize == 4 ? ALIGN_32BIT : (block.accessSize == 2 ? ALIGN_16BIT : ALIGN_8BIT);
		const bool ready = (i > 0 && align == setUpAlign) ? writeRegister(ADIV5_AP_TAR_LOW, block.address) : setupAccess(block.address, align);
		setUpAlign = align;
		data = ready ? readPipelined(block.address, block.count, align, data) : nullptr;
	}
	return checkAccess();
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
