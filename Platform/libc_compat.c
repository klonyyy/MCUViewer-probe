/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * newlib integer-only stdio entry points for PicolibC builds, see libc_compat.h.
 */

#include "libc_compat.h"

#include <stdio.h>

int siprintf(char* const str, const char* const format, ...)
{
	va_list args;
	va_start(args, format);
	const int result = vsprintf(str, format, args);
	va_end(args);
	return result;
}

int sniprintf(char* const str, const size_t size, const char* const format, ...)
{
	va_list args;
	va_start(args, format);
	const int result = vsnprintf(str, size, format, args);
	va_end(args);
	return result;
}

int vsniprintf(char* const str, const size_t size, const char* const format, va_list args)
{
	return vsnprintf(str, size, format, args);
}

int vasiprintf(char** const str, const char* const format, va_list args)
{
	return vasprintf(str, format, args);
}

int siscanf(const char* const str, const char* const format, ...)
{
	va_list args;
	va_start(args, format);
	const int result = vsscanf(str, format, args);
	va_end(args);
	return result;
}
