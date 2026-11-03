/*
 * SPDX-FileCopyrightText: 2026 Piotr Wasilewski
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Force-included into every translation unit that can see BMD headers.
 *
 * BMD's stdio_newlib.h redirects sprintf, snprintf, vsnprintf, vasprintf and sscanf to
 * newlib's integer-only variants. ESP-IDF 6 defaults to PicolibC, which has none of them,
 * so they are declared here and implemented in libc_compat.c. The prototypes match
 * newlib's, so this header is harmless with CONFIG_LIBC_NEWLIB as well.
 */

#ifndef LIBC_COMPAT_H
#define LIBC_COMPAT_H

#include <stdarg.h>
#include <stddef.h>

/* The C++ sources include BMD headers too, which maps their stdio calls to these */
#ifdef __cplusplus
extern "C"
{
#endif

	int siprintf(char* str, const char* format, ...);
	int sniprintf(char* str, size_t size, const char* format, ...);
	int vsniprintf(char* str, size_t size, const char* format, va_list args);
	int vasiprintf(char** str, const char* format, va_list args);
	int siscanf(const char* str, const char* format, ...);

#ifdef __cplusplus
}
#endif

#endif /* LIBC_COMPAT_H */
