/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Black Magic Debug platform definitions for the ESP32-S3 port.
 *
 * BMD includes this header from all of its sources. It deliberately does not include any
 * ESP-IDF header: BMD uses CONFIG_* macros of its own, which must not meet sdkconfig.h.
 */

#ifndef PLATFORM_H
#define PLATFORM_H

#include "timing.h"

#define PLATFORM_IDENT "(NETWORK PROBE ESP32-S3) "

/* The status LED belongs to the application (App/StatusLed), BMD's LED states are not shown */
#define SET_RUN_STATE(state) \
	do                       \
	{                        \
		(void)(state);       \
	} while (0)
#define SET_IDLE_STATE(state) \
	do                        \
	{                         \
		(void)(state);        \
	} while (0)
#define SET_ERROR_STATE(state) \
	do                         \
	{                          \
		(void)(state);         \
	} while (0)

#endif /* PLATFORM_H */
