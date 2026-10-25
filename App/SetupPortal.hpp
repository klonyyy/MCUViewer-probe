/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SETUPPORTAL_HPP
#define SETUPPORTAL_HPP

#include <stddef.h>
#include <stdint.h>

#include <atomic>

#include "esp_http_server.h"
#include "esp_netif.h"

class Wifi;

/**
 * @brief The setup page of setup mode, on the access point: an HTTP server, and a DNS server that answers every name
 * with the access point's address. With the captive portal option of DHCP they make phones and laptops open the page by
 * themselves; any other address redirects to it.
 *
 * The page lists the networks in range, sends the chosen one with its password and the hostname, and polls the status
 * to show how joining went. Its requests never wait for the Wi-Fi task.
 */
class SetupPortal
{
   public:
	explicit SetupPortal(Wifi& wifi) : wifi(wifi) {}

	/**
	 * @brief Starts the servers. Call before the access point starts: its DHCP server takes the captive portal option
	 * only while stopped.
	 */
	void start(esp_netif_t* apNetif);

	void stop();

   private:
	static constexpr uint16_t dnsPort = 53;
	static constexpr size_t dnsMaxQuery = 512;
	static constexpr size_t dnsHeaderSize = 12;
	static constexpr size_t dnsAnswerSize = 16;
	static constexpr uint32_t dnsTtl = 10; /**< Short: clients must not keep the answers after setup. */
	static constexpr uint32_t dnsTaskStackSize = 3072;
	static constexpr uint32_t dnsTaskPriority = 4;
	static constexpr uint32_t httpTaskStackSize = 6144;
	static constexpr uint16_t httpMaxOpenSockets = 3; /**< Fits LWIP_MAX_SOCKETS next to the NETWORK PROBE, GDB and DNS servers. */
	static constexpr size_t maxFormSize = 512;

	static SetupPortal& of(httpd_req_t* request);
	static esp_err_t handlePage(httpd_req_t* request);
	static esp_err_t handleStatus(httpd_req_t* request);
	static esp_err_t handleScan(httpd_req_t* request);
	static esp_err_t handleJoin(httpd_req_t* request);
	static esp_err_t redirect(httpd_req_t* request, httpd_err_code_t error);

	static void dnsTaskEntry(void* arg);
	void runDns();

	/**
	 * @brief Turns the DNS query in packet into its answer, in place: an A question gets the access point's address.
	 * @param packet Room for dnsAnswerSize bytes more than the query.
	 * @return Size of the answer, 0 to drop the query.
	 */
	size_t answerDns(uint8_t* packet, size_t length) const;

	Wifi& wifi;
	httpd_handle_t server = nullptr;
	std::atomic<bool> dnsRunning{false};
	uint32_t apAddress = 0; /**< Network byte order. */
	char pageUrl[24] = {};	/**< http://<access point address>/, also the DHCP captive portal option. */
};

#endif /* SETUPPORTAL_HPP */
