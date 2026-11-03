/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Application.hpp"

/* ESP-IDF's entry point, called from its main task once the system is up */
extern "C" void app_main(void)
{
	app.init();
}
