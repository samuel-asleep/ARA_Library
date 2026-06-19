/* wine_debug_compat.h
 * Provides sprintf_s / vsprintf_s for Wine builds where _WIN32 is defined
 * but the Wine C runtime doesn't expose these MSVC-specific CRT functions.
 * Include via -include flag so it applies to ARADebug.c without modifying it.
 */
#pragma once
#ifdef __WINE__
#include <stdio.h>
#include <stdarg.h>
/* Map MSVC secure CRT to standard POSIX equivalents */
#ifndef sprintf_s
#define sprintf_s(buf, size, ...) snprintf((buf), (size), __VA_ARGS__)
#endif
#ifndef vsprintf_s
#define vsprintf_s(buf, size, fmt, ap) vsnprintf((buf), (size), (fmt), (ap))
#endif
#endif /* __WINE__ */
