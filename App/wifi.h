/*
 * SPDX-FileCopyrightText: 2026 MVProbe contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef WIFI_H
#define WIFI_H

/*
 * Bring up the network: station mode with the configured SSID, or a fallback access point
 * if no SSID is configured or no IP address was obtained in time. Also starts mDNS.
 * Blocks until the network is usable.
 */
void wifi_start(void);

#endif /* WIFI_H */
