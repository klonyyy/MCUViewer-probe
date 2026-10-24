/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APPLICATION_HPP
#define APPLICATION_HPP

#include <stdint.h>

#include "GdbServer.hpp"
#include "HostLink.hpp"
#include "NetServer.hpp"
#include "ProbeService.hpp"
#include "Wifi.hpp"

/**
 * @brief NETWORK PROBE: wireless SWD probe for MCUViewer, built on Black Magic Debug. Owns the network, the net task and the
 * SWD owner task.
 */
class App
{
   public:
	/**
	 * @brief Initializes NVS and the network, then starts the SWD owner task. Called once, from app_main.
	 */
	void init();

   private:
	static constexpr uint32_t probeTaskStackSize = 12288; /**< gdb_main() and the ROM table walk use alloca() and recursion. */
	static constexpr uint32_t probeTaskPriority = 5;
	static constexpr int probeTaskCore = 1; /**< Wi-Fi and lwIP are pinned to core 0, SWD gets core 1 to itself. */

	static void probeTask(void* arg);

	/**
	 * @brief Body of the SWD owner task: serves what the net task hands over, NETWORK PROBE requests and streaming, or a GDB
	 * client. The two never share the SWD lines: the net task hands over a GDB client only while no NETWORK PROBE client is
	 * connected, and answers NETWORK PROBE requests BUSY during a GDB session.
	 */
	[[noreturn]] void runProbe();

	Wifi wifi;
	GdbServer gdbServer;
	HostLink hostLink;
	ProbeService probeService;
	NetServer netServer;
};

extern App app;

#endif /* APPLICATION_HPP */
