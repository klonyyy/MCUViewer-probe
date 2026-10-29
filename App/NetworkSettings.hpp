/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NETWORKSETTINGS_HPP
#define NETWORKSETTINGS_HPP

#include <stddef.h>

/**
 * @brief One network the probe joins, and the hostname to use on it.
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
	 * @return Why the settings cannot be used, or nullptr if they can. The text is shown on the setup page.
	 */
	const char* validate() const;

	/**
	 * @brief The probe's own name: network-probe- and the last three bytes of the station MAC. It names the setup access
	 * point, is the default hostname and the id in the HELLO answer.
	 */
	static void getDeviceId(char* id, size_t size);
};

/**
 * @brief The networks the probe may join, most recently joined first, and its hostname, kept in NVS. Until a network
 * is saved, the Kconfig default network applies; until a hostname is saved, the hostname is the device id.
 */
struct SavedNetworks
{
	static constexpr size_t maxCount = 5;

	NetworkSettings networks[maxCount]; /**< Every entry carries the hostname. */
	size_t count = 0;
	char hostname[NetworkSettings::maxHostnameLength + 1] = {};

	/**
	 * @brief Loads the saved networks, or the Kconfig default network if none is saved.
	 */
	void load();

	/**
	 * @return False if NVS could not be written.
	 */
	bool save() const;

	/**
	 * @brief Puts a network first, replacing the one with the same SSID or else the oldest one if the list is full, and
	 * takes over its hostname. The Kconfig default network is dropped, as it was never saved.
	 * @param settings Valid settings, see NetworkSettings::validate().
	 */
	void add(const NetworkSettings& settings);

	/**
	 * @brief Forgets the saved networks and the hostname: the Kconfig defaults apply again.
	 */
	static void erase();

   private:
	bool defaultOnly = false; /**< The only entry is the Kconfig default, not in NVS. */
};

#endif /* NETWORKSETTINGS_HPP */