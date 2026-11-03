/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TARGETLINK_HPP
#define TARGETLINK_HPP

#include <stddef.h>
#include <stdint.h>

/**
 * @brief The SWD link MCUViewer uses: a light connect and memory access through one MEM-AP while the target runs.
 *
 * Unlike BMD's swd_scan, the light connect never halts the core and writes no core or system register. There is no
 * target_s: the debug port and the access port are set up directly with BMD's low-level ADIv5 functions, the way
 * BMD's remote.c does. ADIv5 debug ports only (DPv1 and single-drop DPv2).
 *
 * There is one SWD interface, so the link is a set of static functions. Call them from the SWD owner task only.
 */
class TargetLink
{
   public:
	/**
	 * @brief Why an operation failed.
	 */
	enum class Error : uint8_t
	{
		none,
		notConnected,  /**< connect() first. */
		noTarget,	   /**< No debug port answered. */
		unsupportedDp, /**< ADIv6 (DPv3) debug port. */
		noPowerUp,	   /**< The debug domain did not acknowledge the power-up request. */
		noMemAp,	   /**< No usable MEM-AP at the requested AP index. */
		swd,		   /**< An SWD access failed, see getFault(). */
	};

	/**
	 * @brief Identification of the connected target.
	 */
	struct ConnectInfo
	{
		uint32_t dpidr;
		uint32_t targetid; /**< DPv2 only, else 0. */
		uint32_t apIdr;
		uint32_t frequency; /**< SWCLK in use, in Hz. */
	};

	/**
	 * @brief Sets the link up: SWD wake-up sequences, debug power-up request, MEM-AP configuration. Sets it up again
	 * when already connected.
	 * @param frequency Requested SWCLK in Hz; the fastest clock at or below it is used.
	 * @param apSel Index of the MEM-AP to use.
	 * @param info Filled on success.
	 * @return True when connected, else see getError().
	 */
	static bool connect(uint32_t frequency, uint8_t apSel, ConnectInfo& info);

	/**
	 * @brief Withdraws the debug power-up request and releases the SWD lines. The target keeps running.
	 */
	static void disconnect();

	/**
	 * @brief Returns true if the link is currently connected.
	 */
	static bool isConnected();

	/**
	 * @brief Reads target memory with the widest access of 4, 2 or 1 bytes that divides both address and length. The
	 * accesses are pipelined: one SWD transaction each, plus a few per 1 KiB page.
	 * @return True on success, else see getError() and getFault().
	 */
	static bool read(uint32_t address, void* data, size_t length);

	/**
	 * @brief Writes target memory, with the same access width rule as read().
	 * @return True on success, else see getError() and getFault().
	 */
	static bool write(uint32_t address, const void* data, size_t length);

	/**
	 * @brief A block of a streaming plan: count accesses of accessSize bytes from consecutive addresses.
	 */
	struct Block
	{
		uint32_t address;
		uint16_t count;
		uint8_t accessSize; /**< 1, 2 or 4. */
	};

	/**
	 * @brief Reads blocks, for example one sample of a plan: every block with exactly its access size, pipelined like
	 * read(), the data packed in block order. The blocks have to be aligned to their access size and must not cross a
	 * 1 KiB boundary.
	 * @return True on success, else see getFault(); the data is then incomplete.
	 */
	static bool readBlocks(const Block* blocks, size_t count, uint8_t* data);

	/**
	 * @brief Resets the target and lets it run: pulses nRST if configured, requests a reset through AIRCR if the core
	 * saw none, then waits for the reset to end.
	 * @return True once the reset ended, else see getError() and getFault().
	 */
	static bool reset();

	static Error getError();

	/**
	 * @brief Fault of the last failed SWD access: the SWD ACK (1 parity error, 2 WAIT, 4 FAULT, 7 no response), 8 for a
	 * sticky error found after the access, 255 for anything else. The values match the protocol's fault codes.
	 */
	static uint8_t getFault();

	/**
	 * @brief Operations failed with Error::swd since boot.
	 */
	static uint32_t getErrorCount();
};

#endif /* TARGETLINK_HPP */
