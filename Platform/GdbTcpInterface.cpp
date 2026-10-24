/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GDB remote serial protocol transport over TCP (one client at a time), BMD's gdb_if.
 *
 * Called only from the task that runs the BMD GDB loop. A lost connection is reported to
 * BMD as EOT ('\x04'), the same way the USB and ctxLink transports report a closed port,
 * so gdb_main() detaches from the target.
 */

#include "GdbTcpInterface.hpp"

#include <errno.h>
#include <stdint.h>

#include <algorithm>

#include "esp_log.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

extern "C"
{
#include "gdb_if.h"
#include "general.h"
}

namespace
{
constexpr const char* tag = "gdb_if";
}  // namespace

GdbTcpInterface gdbTcpInterface;

int GdbTcpInterface::init()
{
	listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listenSocket < 0)
	{
		ESP_LOGE(tag, "socket() failed: errno %d", errno);
		return -1;
	}

	const int one = 1;
	setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_port = htons(CONFIG_NETWORK_PROBE_GDB_PORT);
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(listenSocket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 || listen(listenSocket, 1) != 0)
	{
		ESP_LOGE(tag, "bind/listen on port %d failed: errno %d", CONFIG_NETWORK_PROBE_GDB_PORT, errno);
		close(listenSocket);
		listenSocket = -1;
		return -1;
	}

	ESP_LOGI(tag, "GDB server listening on TCP port %d", CONFIG_NETWORK_PROBE_GDB_PORT);
	return 0;
}

void GdbTcpInterface::disconnectClient()
{
	if (clientSocket >= 0)
	{
		shutdown(clientSocket, SHUT_RDWR);
		close(clientSocket);
		ESP_LOGI(tag, "GDB client disconnected");
	}
	clientSocket = -1;
	rxLength = 0;
	rxIndex = 0;
	txLength = 0;
	eotPending = true;
}

bool GdbTcpInterface::acceptClient()
{
	if (clientSocket >= 0)
		return true;

	sockaddr_in peer;
	socklen_t peerLength = sizeof(peer);
	const int sock = accept(listenSocket, reinterpret_cast<sockaddr*>(&peer), &peerLength);
	if (sock < 0)
	{
		ESP_LOGW(tag, "accept() failed: errno %d", errno);
		return false;
	}

	const int one = 1;
	const int keepaliveIdle = 5;
	const int keepaliveInterval = 2;
	const int keepaliveCount = 3;
	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
	setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepaliveIdle, sizeof(keepaliveIdle));
	setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepaliveInterval, sizeof(keepaliveInterval));
	setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepaliveCount, sizeof(keepaliveCount));

	char peerName[16];
	inet_ntoa_r(peer.sin_addr, peerName, sizeof(peerName));
	ESP_LOGI(tag, "GDB client connected from %s", peerName);
	clientSocket = sock;
	eotPending = false;
	return true;
}

bool GdbTcpInterface::receive(const int32_t timeoutMs)
{
	if (timeoutMs >= 0)
	{
		fd_set readSet;
		FD_ZERO(&readSet);
		FD_SET(clientSocket, &readSet);
		timeval timeout = {
			.tv_sec = timeoutMs / 1000,
			.tv_usec = (timeoutMs % 1000) * 1000,
		};
		const int ready = select(clientSocket + 1, &readSet, nullptr, nullptr, &timeout);
		if (ready == 0)
			return false;
		if (ready < 0)
		{
			disconnectClient();
			return false;
		}
	}

	const ssize_t received = recv(clientSocket, rxBuffer.data(), rxBuffer.size(), 0);
	if (received <= 0)
	{
		disconnectClient();
		return false;
	}
	rxLength = static_cast<size_t>(received);
	rxIndex = 0;
	return true;
}

char GdbTcpInterface::getChar()
{
	while (true)
	{
		if (eotPending)
		{
			eotPending = false;
			return '\x04';
		}
		if (rxIndex < rxLength)
			return rxBuffer[rxIndex++];
		/* The session is over: the SWD owner task accepts the next client itself */
		if (clientSocket < 0)
			return '\x04';
		receive(-1);
	}
}

char GdbTcpInterface::getCharTimeout(const uint32_t timeout)
{
	/* Keep the EOT pending: the GDB loop still needs it to detach once the target stops */
	if (eotPending)
		return '\x04';
	if (rxIndex < rxLength)
		return rxBuffer[rxIndex++];
	if (clientSocket < 0)
		return static_cast<char>(-1);
	if (!receive(static_cast<int32_t>(std::min<uint32_t>(timeout, INT32_MAX))))
		return eotPending ? '\x04' : static_cast<char>(-1);
	return rxBuffer[rxIndex++];
}

void GdbTcpInterface::flush()
{
	if (clientSocket < 0)
	{
		txLength = 0;
		return;
	}

	size_t sent = 0;
	while (sent < txLength)
	{
		const ssize_t result = send(clientSocket, txBuffer.data() + sent, txLength - sent, 0);
		if (result < 0)
		{
			if (errno == EINTR || errno == EAGAIN)
				continue;
			disconnectClient();
			return;
		}
		sent += static_cast<size_t>(result);
	}
	txLength = 0;
}

void GdbTcpInterface::putChar(const char c, const bool flushNow)
{
	if (clientSocket < 0)
		return;
	txBuffer[txLength++] = c;
	if (flushNow || txLength == txBuffer.size())
		flush();
}

int gdb_if_init(void)
{
	return gdbTcpInterface.init();
}

char gdb_if_getchar(void)
{
	return gdbTcpInterface.getChar();
}

char gdb_if_getchar_to(const uint32_t timeout)
{
	return gdbTcpInterface.getCharTimeout(timeout);
}

void gdb_if_flush([[maybe_unused]] const bool force)
{
	gdbTcpInterface.flush();
}

void gdb_if_putchar(const char c, const bool flush)
{
	gdbTcpInterface.putChar(c, flush);
}
