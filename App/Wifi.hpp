/*
 * SPDX-FileCopyrightText: 2026 MVProbe contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef WIFI_HPP
#define WIFI_HPP

#include <stdint.h>

#include <atomic>

#include "esp_bit_defs.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

/**
 * @brief Wi-Fi station with access point fallback, and the mDNS announcement.
 */
class Wifi
{
   public:
	/**
	 * @brief Brings up the network: station mode with the configured SSID, or a fallback access point if no SSID is
	 * configured or no IP address was obtained in time. Also starts mDNS. Blocks until the network is usable.
	 */
	void start();

   private:
	static constexpr EventBits_t gotIpBit = BIT0;
	static constexpr uint8_t apMaxConnections = 2;

	static void eventHandler(void* arg, esp_event_base_t base, int32_t id, void* data);
	void handleEvent(esp_event_base_t base, int32_t id, void* data);

	/**
	 * @brief Joins the configured network.
	 * @return True once an IP address was obtained, false if none came within the timeout: the station is stopped then.
	 */
	bool startStation();
	void startAccessPoint();
	void startMdns();

	EventGroupHandle_t events = nullptr;
	esp_netif_t* staNetif = nullptr;
	esp_netif_t* apNetif = nullptr;
	std::atomic<bool> staReconnect{true}; /**< Reconnect on disconnection unless the probe gave up and switched to AP mode. */
};

#endif /* WIFI_HPP */
