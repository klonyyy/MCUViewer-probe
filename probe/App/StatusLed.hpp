/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef STATUSLED_HPP
#define STATUSLED_HPP

#include <stdint.h>

/**
 * @brief The status LED: a WS2812 RGB LED, driven by RMT.
 *
 * It shows the first of these that applies:
 * - the setup button held: white blinks that speed up for 3 s, then steady white (released now, the saved networks are
 *   forgotten, or field mode left), from 10 s fast red blinks (released now, the pairings may be erased);
 * - the pairing erase armed: red and white by turns, until a short press confirms it or 5 s pass;
 * - a pairing done: three green flashes;
 * - a pairing request: yellow blinks, until the BOOT button accepts it or 30 s pass;
 * - identification: fast white blinks for a few seconds, when a host asked which probe this is;
 * - an alert: one amber flash when samples or SWO bytes were dropped, one red flash when an access to the target
 *   failed, red blinks when the target was lost or a Wi-Fi join failed;
 * - a GDB session: orange, dipping briefly for the packets;
 * - an MCUViewer session: cyan, dipping briefly for the frames sent, so streaming flickers evenly;
 * - the network: blue blinks while joining, purple breathing in setup mode, slow green breathing once online, slow
 *   blue breathing in field mode.
 *
 * The other modules only record what happened, from any task; the LED task on core 0 renders it 50 times a second.
 * There is one LED, so the class is a set of static functions.
 */
class StatusLed
{
   public:
	enum class Network : uint8_t
	{
		connecting, /**< Joining a network, no address yet. */
		setup,		/**< Setup mode: the access point is up. */
		online,		/**< The station has an address. */
		field,		/**< Field mode: no network, ESP-NOW alone. */
	};

	/**
	 * @brief In rising severity: a more severe alert cuts a running one short, an equal or lesser one is dropped.
	 */
	enum class Alert : uint8_t
	{
		none,
		warning, /**< Data was dropped: one amber flash. */
		error,	 /**< An access to the target failed: one red flash. */
		failure, /**< The target was lost, or a join failed: red blinks. */
	};

	/**
	 * @brief Starts the LED task. Call once, before the other modules start. Without a GPIO configured it does nothing,
	 * and the other functions are harmless.
	 */
	static void start();

	static void setNetwork(Network network);
	static void setHostConnected(bool connected);
	static void setGdbConnected(bool connected);

	/** What releasing the held setup button would do. */
	enum class Hold : uint8_t
	{
		none,	  /**< Released. */
		counting, /**< Nothing yet. */
		networks, /**< Forget the saved networks, or leave field mode. */
		erase,	  /**< Arm the pairing erase. */
	};

	/**
	 * @brief Shows that the setup button is held.
	 * @param remainingMs While counting: until the hold reaches networks.
	 */
	static void setButtonHold(Hold hold, uint32_t remainingMs = 0);

	/**
	 * @brief The pairing erase waits for a short press, or not any more.
	 */
	static void setEraseArmed(bool armed);

	/**
	 * @brief A dongle waits for the BOOT button to accept its pairing, or not any more.
	 */
	static void setPairingRequest(bool waiting);

	/**
	 * @brief A dongle got paired: three green flashes.
	 */
	static void paired();

	/**
	 * @brief Blinks the LED white for a few seconds, so the user can tell this probe from the others. A call while it
	 * blinks makes it blink longer.
	 */
	static void identify();

	/**
	 * @brief Marks traffic of the session: a frame or a packet. A burst shows as one dip.
	 */
	static void activity();

	static void alert(Alert alert);
};

#endif /* STATUSLED_HPP */
