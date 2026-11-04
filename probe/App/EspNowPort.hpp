/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ESPNOWPORT_HPP
#define ESPNOWPORT_HPP

#include <stddef.h>
#include <stdint.h>

#include "EspNowLink.hpp"

/**
 * @brief The probe's ESP-NOW port, for the net task: packets come in through a pool the Wi-Fi task fills, and go out
 * a few at a time, each send callback freeing a place. Peers are added as they are sent to, at the link's PHY rate.
 * ESP-NOW only runs while Wi-Fi does, so the Wi-Fi task suspends the port around stopping Wi-Fi.
 *
 * There is one port, so the class is a set of static functions.
 */
class EspNowPort
{
   public:
	struct Packet
	{
		uint8_t mac[6];
		int8_t rssi;
		uint16_t length;
		uint8_t data[EspNowLink::maxPacket];
	};

	enum class SendResult : uint8_t
	{
		sent,
		busy,	/**< The window is full or ESP-NOW is out of buffers: try again when the net task wakes. */
		failed, /**< ESP-NOW refused the packet. */
	};

	/**
	 * @brief Starts ESP-NOW. Call once, from the net task, while Wi-Fi runs.
	 * @param wakeFd eventfd written when a packet came in or a send finished.
	 */
	static void start(int wakeFd);

	/**
	 * @brief Wi-Fi task: stops ESP-NOW before esp_wifi_stop(), and starts it again after esp_wifi_start(). Harmless
	 * before start(). The peers are gone after it, and come back as they are sent to.
	 */
	static void suspend();
	static void resume();

	/**
	 * @return The next packet received, or nullptr. Give it back with release().
	 */
	static Packet* receive();
	static void release(Packet* packet);

	/**
	 * @return Whether the window has room for a packet.
	 */
	static bool canSend();

	/**
	 * @brief Sends a packet: the link header, then frame, which may be empty.
	 */
	static SendResult send(const uint8_t* mac, EspNowLink::Kind kind, uint16_t session, const uint8_t* frame, size_t length);
};

#endif /* ESPNOWPORT_HPP */
