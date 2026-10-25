/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NETWORKSETTINGS_HPP
#define NETWORKSETTINGS_HPP

#include <stddef.h>

/**
 * @brief The network the probe joins and its hostname, kept in NVS. Until a network is saved, the Kconfig defaults
 * apply; until a hostname is saved, the hostname is the device id.
 */
struct NetworkSettings
{
	static constexpr size_t maxSsidLength = 32;
	static constexpr size_t maxPasswordLength = 64; /**< A passphrase of up to 63 characters, or a PSK of 64 hex digits. */
	static constexpr size_t maxHostnameLength = 32;
	static constexpr size_t deviceIdSize = 21; /**< "network-probe-", six hex digits and the terminator. */

	char ssid[maxSsidLength + 1] = {};
	char password[maxPasswordLength + 1] = {};
	char hostname[maxHostnameLength + 1] = {};

	/**
	 * @brief Loads the saved settings, or the Kconfig defaults if no network is saved.
	 */
	void load();

	/**
	 * @return False if NVS could not be written.
	 */
	bool save() const;

	/**
	 * @return Why the settings cannot be used, or nullptr if they can. The text is shown on the setup page.
	 */
	const char* validate() const;

	/**
	 * @brief Forgets the saved settings: the Kconfig defaults apply again.
	 */
	static void erase();

	/**
	 * @brief The probe's own name: network-probe- and the last three bytes of the station MAC. It names the setup access
	 * point, is the default hostname and the id in the HELLO answer.
	 */
	static void getDeviceId(char* id, size_t size);
};

#endif /* NETWORKSETTINGS_HPP */
