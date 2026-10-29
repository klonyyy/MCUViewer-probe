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

constexpr size_t keySize = 16;

/* The first network keeps the keys of the single-network firmware, so a probe keeps its network when updated */
void slotKeys(const size_t slot, char* const ssidKey, char* const passwordKey)
{
	if (slot == 0)
	{
		strlcpy(ssidKey, "ssid", keySize);
		strlcpy(passwordKey, "password", keySize);
		return;
	}
	snprintf(ssidKey, keySize, "ssid%u", static_cast<unsigned>(slot));
	snprintf(passwordKey, keySize, "password%u", static_cast<unsigned>(slot));
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

void SavedNetworks::load()
{
	*this = SavedNetworks{};

	nvs_handle_t handle;
	if (nvs_open(nvsNamespace, NVS_READONLY, &handle) == ESP_OK)
	{
		for (; count < maxCount; ++count)
		{
			char ssidKey[keySize];
			char passwordKey[keySize];
			slotKeys(count, ssidKey, passwordKey);
			NetworkSettings& network = networks[count];
			if (!getString(handle, ssidKey, network.ssid, sizeof(network.ssid)) || !network.ssid[0])
				break;
			getString(handle, passwordKey, network.password, sizeof(network.password));
		}
		getString(handle, "hostname", hostname, sizeof(hostname));
		nvs_close(handle);
	}
	if (!count && CONFIG_NETWORK_PROBE_WIFI_SSID[0])
	{
		strlcpy(networks[0].ssid, CONFIG_NETWORK_PROBE_WIFI_SSID, sizeof(networks[0].ssid));
		strlcpy(networks[0].password, CONFIG_NETWORK_PROBE_WIFI_PASSWORD, sizeof(networks[0].password));
		count = 1;
		defaultOnly = true;
	}
	if (!hostname[0])
		NetworkSettings::getDeviceId(hostname, sizeof(hostname));
	for (size_t i = 0; i < count; ++i)
		strlcpy(networks[i].hostname, hostname, sizeof(networks[i].hostname));
}

bool SavedNetworks::save() const
{
	nvs_handle_t handle;
	if (nvs_open(nvsNamespace, NVS_READWRITE, &handle) != ESP_OK)
		return false;
	bool written = nvs_set_str(handle, "hostname", hostname) == ESP_OK;
	for (size_t i = 0; i < maxCount && written; ++i)
	{
		char ssidKey[keySize];
		char passwordKey[keySize];
		slotKeys(i, ssidKey, passwordKey);
		if (i < count)
			written = nvs_set_str(handle, ssidKey, networks[i].ssid) == ESP_OK && nvs_set_str(handle, passwordKey, networks[i].password) == ESP_OK;
		else
		{
			const esp_err_t ssidResult = nvs_erase_key(handle, ssidKey);
			const esp_err_t passwordResult = nvs_erase_key(handle, passwordKey);
			written = (ssidResult == ESP_OK || ssidResult == ESP_ERR_NVS_NOT_FOUND) && (passwordResult == ESP_OK || passwordResult == ESP_ERR_NVS_NOT_FOUND);
		}
	}
	written = written && nvs_commit(handle) == ESP_OK;
	nvs_close(handle);
	return written;
}

void SavedNetworks::add(const NetworkSettings& settings)
{
	if (defaultOnly)
	{
		count = 0;
		defaultOnly = false;
	}

	size_t position = count;
	for (size_t i = 0; i < count; ++i)
		if (!strcmp(networks[i].ssid, settings.ssid))
			position = i;
	if (position == maxCount)
		--position;
	else if (position == count)
		++count;

	for (size_t i = position; i > 0; --i)
		networks[i] = networks[i - 1];
	networks[0] = settings;

	strlcpy(hostname, settings.hostname, sizeof(hostname));
	for (size_t i = 0; i < count; ++i)
		strlcpy(networks[i].hostname, hostname, sizeof(networks[i].hostname));
}

void SavedNetworks::erase()
{
	nvs_handle_t handle;
	if (nvs_open(nvsNamespace, NVS_READWRITE, &handle) != ESP_OK)
		return;
	if (nvs_erase_all(handle) != ESP_OK || nvs_commit(handle) != ESP_OK)
		ESP_LOGE(tag, "erasing the network settings failed");
	nvs_close(handle);
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

void NetworkSettings::getDeviceId(char* const id, const size_t size)
{
	uint8_t mac[6] = {};
	esp_read_mac(mac, ESP_MAC_WIFI_STA);
	snprintf(id, size, "network-probe-%02x%02x%02x", mac[3], mac[4], mac[5]);
}
