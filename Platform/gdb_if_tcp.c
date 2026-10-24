/*
 * SPDX-FileCopyrightText: 2026 MVProbe contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GDB remote serial protocol transport over TCP (one client at a time).
 *
 * Called only from the task that runs the BMD GDB loop. A lost connection is reported to
 * BMD as EOT ('\x04'), the same way the USB and ctxLink transports report a closed port,
 * so gdb_main() detaches from the target.
 */

#include "general.h"
#include "gdb_if.h"

#include <errno.h>
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sdkconfig.h"

#define GDB_IF_BUFFER_SIZE 1024U

static const char *const TAG = "gdb_if";

static int listen_socket = -1;
static int client_socket = -1;
/* Set when the client goes away, cleared once gdb_if_getchar() has delivered the EOT */
static bool eot_pending = false;

static char rx_buffer[GDB_IF_BUFFER_SIZE];
static size_t rx_length = 0;
static size_t rx_index = 0;

static char tx_buffer[GDB_IF_BUFFER_SIZE];
static size_t tx_length = 0;

int gdb_if_init(void)
{
	listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listen_socket < 0) {
		ESP_LOGE(TAG, "socket() failed: errno %d", errno);
		return -1;
	}

	const int one = 1;
	setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	const struct sockaddr_in address = {
		.sin_family = AF_INET,
		.sin_port = htons(CONFIG_MVPROBE_GDB_PORT),
		.sin_addr.s_addr = htonl(INADDR_ANY),
	};
	if (bind(listen_socket, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
		listen(listen_socket, 1) != 0) {
		ESP_LOGE(TAG, "bind/listen on port %d failed: errno %d", CONFIG_MVPROBE_GDB_PORT, errno);
		close(listen_socket);
		listen_socket = -1;
		return -1;
	}

	ESP_LOGI(TAG, "GDB server listening on TCP port %d", CONFIG_MVPROBE_GDB_PORT);
	return 0;
}

static void gdb_if_disconnect(void)
{
	if (client_socket >= 0) {
		shutdown(client_socket, SHUT_RDWR);
		close(client_socket);
		ESP_LOGI(TAG, "GDB client disconnected");
	}
	client_socket = -1;
	rx_length = 0;
	rx_index = 0;
	tx_length = 0;
	eot_pending = true;
}

static void gdb_if_accept(void)
{
	while (client_socket < 0) {
		struct sockaddr_in peer;
		socklen_t peer_length = sizeof(peer);
		const int sock = accept(listen_socket, (struct sockaddr *)&peer, &peer_length);
		if (sock < 0) {
			ESP_LOGW(TAG, "accept() failed: errno %d", errno);
			vTaskDelay(pdMS_TO_TICKS(100));
			continue;
		}

		const int one = 1;
		const int keepalive_idle = 5;
		const int keepalive_interval = 2;
		const int keepalive_count = 3;
		setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
		setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepalive_idle, sizeof(keepalive_idle));
		setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepalive_interval, sizeof(keepalive_interval));
		setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepalive_count, sizeof(keepalive_count));

		char peer_name[16];
		inet_ntoa_r(peer.sin_addr, peer_name, sizeof(peer_name));
		ESP_LOGI(TAG, "GDB client connected from %s", peer_name);
		client_socket = sock;
	}
}

/*
 * Refill the receive buffer. timeout_ms < 0 blocks.
 * Returns false on timeout or when the connection was lost.
 */
static bool gdb_if_receive(const int32_t timeout_ms)
{
	if (timeout_ms >= 0) {
		fd_set read_set;
		FD_ZERO(&read_set);
		FD_SET(client_socket, &read_set);
		struct timeval timeout = {
			.tv_sec = timeout_ms / 1000,
			.tv_usec = (timeout_ms % 1000) * 1000,
		};
		const int ready = select(client_socket + 1, &read_set, NULL, NULL, &timeout);
		if (ready == 0)
			return false;
		if (ready < 0) {
			gdb_if_disconnect();
			return false;
		}
	}

	const ssize_t received = recv(client_socket, rx_buffer, sizeof(rx_buffer), 0);
	if (received <= 0) {
		gdb_if_disconnect();
		return false;
	}
	rx_length = (size_t)received;
	rx_index = 0;
	return true;
}

char gdb_if_getchar(void)
{
	while (true) {
		if (eot_pending) {
			eot_pending = false;
			return '\x04';
		}
		if (rx_index < rx_length)
			return rx_buffer[rx_index++];
		if (client_socket < 0)
			gdb_if_accept();
		else
			gdb_if_receive(-1);
	}
}

char gdb_if_getchar_to(const uint32_t timeout)
{
	/* Keep the EOT pending: the GDB loop still needs it to detach once the target stops */
	if (eot_pending)
		return '\x04';
	if (rx_index < rx_length)
		return rx_buffer[rx_index++];
	if (client_socket < 0)
		return (char)-1;
	if (!gdb_if_receive((int32_t)MIN(timeout, (uint32_t)INT32_MAX)))
		return eot_pending ? '\x04' : (char)-1;
	return rx_buffer[rx_index++];
}

void gdb_if_flush(const bool force)
{
	(void)force;
	if (client_socket < 0) {
		tx_length = 0;
		return;
	}

	size_t sent = 0;
	while (sent < tx_length) {
		const ssize_t result = send(client_socket, tx_buffer + sent, tx_length - sent, 0);
		if (result < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			gdb_if_disconnect();
			return;
		}
		sent += (size_t)result;
	}
	tx_length = 0;
}

void gdb_if_putchar(const char c, const bool flush)
{
	if (client_socket < 0)
		return;
	tx_buffer[tx_length++] = c;
	if (flush || tx_length == sizeof(tx_buffer))
		gdb_if_flush(flush);
}
