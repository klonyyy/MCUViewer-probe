/*
 * SPDX-FileCopyrightText: 2026 MVProbe contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APPLICATION_HPP
#define APPLICATION_HPP

#include <stdint.h>

#include "GdbServer.hpp"
#include "Wifi.hpp"

/**
 * @brief MVProbe: wireless SWD probe for MCUViewer, built on Black Magic Debug. Owns the network and the SWD owner
 * task.
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

	Wifi wifi;
	GdbServer gdbServer;
};

extern App app;

#endif /* APPLICATION_HPP */
