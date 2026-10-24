/*
 * SPDX-FileCopyrightText: 2026 MVProbe contributors
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

#define PLATFORM_IDENT "(MVProbe ESP32-S3) "

/* Extra monitor commands, see platform_cmds.c */
#define PLATFORM_HAS_CUSTOM_COMMANDS

/* No status LEDs yet */
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
