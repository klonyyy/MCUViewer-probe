/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: MIT
 */

#include "Dongle.hpp"

namespace
{
Dongle dongle;
}  // namespace

extern "C" void app_main()
{
	dongle.start();
}
