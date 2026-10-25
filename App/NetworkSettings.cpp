/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "NetworkSettings.hpp"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "sdkconfig.h"

namespace
{
constexpr const char* tag = "settings";
constexpr const char* nvsNamespace = "network";

bool getString(const nvs_handle_t handle, const char* const key, char* const out, const size_t size)
{
	size_t length = size;
	if (nvs_get_str(handle, key, out, &length) == ESP_OK)
		return true;
	out[0] = '\0';
	return false;
}

bool isHex(const char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool isHostnameChar(const char c)
{
	return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
}
}  // namespace

void NetworkSettings::load()
{
	*this = NetworkSettings{};

	bool saved = false;
	nvs_handle_t handle;
	if (nvs_open(nvsNamespace, NVS_READONLY, &handle) == ESP_OK)
	{
		saved = getString(handle, "ssid", ssid, sizeof(ssid)) && ssid[0];
		getString(handle, "password", password, sizeof(password));
		getString(handle, "hostname", hostname, sizeof(hostname));
		nvs_close(handle);
	}
	if (!saved)
	{
		strlcpy(ssid, CONFIG_NETWORK_PROBE_WIFI_SSID, sizeof(ssid));
		strlcpy(password, CONFIG_NETWORK_PROBE_WIFI_PASSWORD, sizeof(password));
	}
	if (!hostname[0])
		getDeviceId(hostname, sizeof(hostname));
}

bool NetworkSettings::save() const
{
	nvs_handle_t handle;
	if (nvs_open(nvsNamespace, NVS_READWRITE, &handle) != ESP_OK)
		return false;
	const bool written = nvs_set_str(handle, "ssid", ssid) == ESP_OK && nvs_set_str(handle, "password", password) == ESP_OK &&
						 nvs_set_str(handle, "hostname", hostname) == ESP_OK && nvs_commit(handle) == ESP_OK;
	nvs_close(handle);
	return written;
}

const char* NetworkSettings::validate() const
{
	if (!ssid[0])
		return "Choose a network or type its name.";

	const size_t passwordLength = strlen(password);
	if (passwordLength > 0 && passwordLength < 8)
		return "A Wi-Fi password has at least 8 characters.";
	if (passwordLength == maxPasswordLength)
	{
		for (size_t i = 0; i < passwordLength; ++i)
			if (!isHex(password[i]))
				return "A password of 64 characters must be a key of hex digits; a passphrase has at most 63.";
	}

	const size_t hostnameLength = strlen(hostname);
	bool hostnameValid = hostnameLength > 0 && hostname[0] != '-' && hostname[hostnameLength - 1] != '-';
	for (size_t i = 0; i < hostnameLength; ++i)
		hostnameValid = hostnameValid && isHostnameChar(hostname[i]);
	if (!hostnameValid)
		return "A hostname has only the letters a-z, digits and '-', and does not start or end with '-'.";

	return nullptr;
}

void NetworkSettings::erase()
{
	nvs_handle_t handle;
	if (nvs_open(nvsNamespace, NVS_READWRITE, &handle) != ESP_OK)
		return;
	if (nvs_erase_all(handle) != ESP_OK || nvs_commit(handle) != ESP_OK)
		ESP_LOGE(tag, "erasing the network settings failed");
	nvs_close(handle);
}

void NetworkSettings::getDeviceId(char* const id, const size_t size)
{
	uint8_t mac[6] = {};
	esp_read_mac(mac, ESP_MAC_WIFI_STA);
	snprintf(id, size, "network-probe-%02x%02x%02x", mac[3], mac[4], mac[5]);
}
