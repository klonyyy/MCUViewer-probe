/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GDBSERVER_HPP
#define GDBSERVER_HPP

/**
 * @brief BMD's GDB server, run by the SWD owner task one client session at a time.
 */
class GdbServer
{
   public:
	/**
	 * @brief Opens the listening socket, retrying until it succeeds. Call once, after platform_init().
	 */
	void init();

	/**
	 * @return The listening socket, readable when a client is waiting.
	 */
	int getListenSocket() const;

	/**
	 * @brief Accepts the waiting client and serves it until it disconnects and BMD has detached from the target. Call
	 * from the task that owns SWD.
	 */
	void serve();

   private:
	/**
	 * @brief Polls a running target until it stops or GDB interrupts it, then handles one GDB packet.
	 */
	void pollLoop();
};

#endif /* GDBSERVER_HPP */
