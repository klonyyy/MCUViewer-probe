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
 * - the setup button held: white blinks that speed up until the saved networks are forgotten;
 * - identification: fast white blinks for a few seconds, when a host asked which probe this is;
 * - an alert: one amber flash when samples or SWO bytes were dropped, one red flash when an access to the target
 *   failed, red blinks when the target was lost or a Wi-Fi join failed;
 * - a GDB session: orange, dipping briefly for the packets;
 * - an MCUViewer session: cyan, dipping briefly for the frames sent, so streaming flickers evenly;
 * - the network: blue blinks while joining, purple breathing in setup mode, slow green breathing once online.
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

	/**
	 * @brief Shows that the setup button is held.
	 * @param remainingMs Until the hold takes effect; 0 when the button is released.
	 */
	static void setButtonHold(uint32_t remainingMs);

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
