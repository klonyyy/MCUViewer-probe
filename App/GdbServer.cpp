/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The loop is adapted from src/main.c of Black Magic Debug:
 *   Copyright (C) 2011  Black Sphere Technologies Ltd.
 *   Written by Gareth McMullin <gareth@blacksphere.co.nz>
 * It is reimplemented here instead of linking main.c, which defines main() and never
 * returns, so that the probe can leave GDB mode between sessions and serve MCUViewer.
 */

#include "GdbServer.hpp"

#include "GdbTcpInterface.hpp"

extern "C"
{
#include "exception.h"
#include "gdb_if.h"
#include "gdb_main.h"
#include "gdb_packet.h"
#include "general.h"
#include "morse.h"
#include "platform.h"
#include "target.h"
}

void GdbServer::init()
{
	while (gdb_if_init() != 0)
		platform_delay(1000);
}

int GdbServer::getListenSocket() const
{
	return gdbTcpInterface.getListenSocket();
}

void GdbServer::serve()
{
	if (!gdbTcpInterface.acceptClient())
		return;

	while (gdbTcpInterface.isSessionOpen())
	{
		TRY(EXCEPTION_ALL)
		{
			pollLoop();
		}
		CATCH()
		{
			default:
				gdb_put_packet_error(0xffU);
				target_list_free();
				gdb_outf("Uncaught exception: %s\n", exception_frame.msg);
				morse("TARGET LOST.", true);
		}
	}
}

void GdbServer::pollLoop()
{
	SET_IDLE_STATE(false);
	while (gdb_target_running && cur_target)
	{
		gdb_poll_target();

		/* Check again, as gdb_poll_target() may alter these variables */
		if (!gdb_target_running || !cur_target)
			break;
		const char c = gdb_if_getchar_to(0);
		if (c == '\x03' || c == '\x04')
			target_halt_request(cur_target);
		platform_pace_poll();
	}

	SET_IDLE_STATE(true);
	const gdb_packet_s* const packet = gdb_packet_receive();
	/* If port closed and target detached, stay idle */
	if (packet->data[0] != '\x04' || cur_target)
		SET_IDLE_STATE(false);
	gdb_main(packet);
}
