/*
 * SPDX-FileCopyrightText: 2026 MVProbe contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GDBSERVER_HPP
#define GDBSERVER_HPP

/**
 * @brief BMD's GDB server, run by the SWD owner task.
 */
class GdbServer
{
   public:
	/**
	 * @brief Initializes the BMD platform layer and serves GDB forever. Call from the task pinned to the core that owns
	 * SWD.
	 */
	[[noreturn]] void run();

   private:
	/**
	 * @brief Polls a running target until it stops or GDB interrupts it, then handles one GDB packet.
	 */
	void pollLoop();
};

#endif /* GDBSERVER_HPP */
