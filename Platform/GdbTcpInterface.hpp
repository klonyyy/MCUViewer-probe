/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GDBTCPINTERFACE_HPP
#define GDBTCPINTERFACE_HPP

#include <stddef.h>
#include <stdint.h>

#include <array>

/**
 * @brief GDB remote serial protocol transport over TCP, one client at a time: the listening socket and the client
 * connection, with BMD's character I/O (gdb_if) buffered on top.
 *
 * The SWD owner task waits for a client on getListenSocket() itself, so it can also wait for other protocols, and
 * accepts it with acceptClient(). A session lasts until the client went away and BMD has seen the EOT.
 */
class GdbTcpInterface
{
   public:
	/**
	 * @return 0 once the server listens, -1 on failure.
	 */
	int init();

	/**
	 * @brief Returns the listening socket file descriptor.
	 */
	int getListenSocket() const { return listenSocket; }

	/**
	 * @brief Accepts a pending client. Call when the listening socket is readable.
	 * @return True when a client is connected.
	 */
	bool acceptClient();

	/**
	 * @brief A session is open while a client is connected and until getChar() has delivered the EOT that ends it.
	 */
	bool isSessionOpen() const { return clientSocket >= 0 || eotPending; }

	/**
	 * @brief Waits for the next character.
	 * @return The character, or EOT ('\x04') once the client went away.
	 */
	char getChar();

	/**
	 * @return The next character, EOT while the client is gone, or -1 if nothing came within timeout milliseconds.
	 */
	char getCharTimeout(uint32_t timeout);

	/**
	 * @brief Sends a character to the client.
	 * @param c The character to send.
	 * @param flushNow If true, flushes the transmit buffer immediately.
	 */
	void putChar(char c, bool flushNow);

	/**
	 * @brief Flushes the transmit buffer to the client.
	 */
	void flush();

   private:
	static constexpr size_t bufferSize = 1024;

	void disconnectClient();

	/**
	 * @brief Refills the receive buffer. timeoutMs < 0 blocks.
	 * @return False on timeout or when the connection was lost.
	 */
	bool receive(int32_t timeoutMs);

	int listenSocket = -1;
	int clientSocket = -1;
	bool eotPending = false; /**< Set when the client goes away, cleared once getChar() has delivered the EOT. */

	std::array<char, bufferSize> rxBuffer;
	size_t rxLength = 0;
	size_t rxIndex = 0;

	std::array<char, bufferSize> txBuffer;
	size_t txLength = 0;
};

extern GdbTcpInterface gdbTcpInterface;

#endif /* GDBTCPINTERFACE_HPP */
