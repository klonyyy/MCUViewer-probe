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
 * one at a time, as EspNowLink asks: a session packet the radio got no acknowledgement for goes again, from service(),
 * after a quiet pause at the lowest rate that a packet from its peer cuts short; an answer outside a session and a
 * CLOSE do not. Peers are added as they are sent to, and each has a rate ladder (EspNowRate.hpp), which the outcome of
 * every session packet to it moves; broadcasts go at the base rate. The paired dongles are pinned: encrypted peers that
 * stay, so their packets can be decrypted before the probe ever sends to them. ESP-NOW only runs while Wi-Fi does, so
 * the Wi-Fi task suspends the port around stopping Wi-Fi; the pinned peers come back after it.
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
		uint8_t channel; /**< The channel the radio was on when the packet came. */
		uint16_t length;
		uint8_t data[EspNowLink::maxPacket];
	};

	enum class SendResult : uint8_t
	{
		sent,
		busy,	/**< A packet is still in the air, or ESP-NOW is out of buffers: try again when the net task wakes. */
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
	 * @brief Net task, every round: moves the rate ladder of the last packet's peer, and sends again a packet the radio
	 * got no acknowledgement for, or gives it up.
	 */
	static void service();

	/**
	 * @return The next packet received, or nullptr. Give it back with release().
	 */
	static Packet* receive();
	static void release(Packet* packet);

	/**
	 * @return Whether a packet can go now: none is in the air.
	 */
	static bool canSend();

	/**
	 * @brief A session with mac starts: the packets to it start at the top of the ladder again.
	 */
	static void resetRate(const uint8_t* mac);

	/**
	 * @return The rate the packets to mac go at now, in 0.5 Mb/s; 0 for a peer not sent to lately.
	 */
	static uint8_t getRate(const uint8_t* mac);

	/**
	 * @brief The session ended: a packet of it waiting to go again is dropped, and one in the air is not sent again.
	 */
	static void abandon();

	/**
	 * @brief Makes mac an encrypted peer with lmk, for good: a paired dongle.
	 * @return False when ESP-NOW refused it.
	 */
	static bool pin(const uint8_t* mac, const uint8_t* lmk);

	/**
	 * @brief Makes a pinned peer an ordinary one again: the dongle was forgotten.
	 */
	static void unpin(const uint8_t* mac);

	/**
	 * @return This probe's MAC address on ESP-NOW.
	 */
	static const uint8_t* getOwnMac();

	/**
	 * @brief Sends a packet: the link header, then frame, which may be empty. The header's channel is filled in: the
	 * one the radio is on.
	 * @param once No resends, and the rate ladder stays: for a CLOSE. Answers outside a session always go once.
	 */
	static SendResult send(const uint8_t* mac, const EspNowLink::Header& header, const uint8_t* frame, size_t length, bool once = false);
};

#endif /* ESPNOWPORT_HPP */
