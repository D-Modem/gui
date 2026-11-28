/*
 * time.h for Mac OS X 10.4 (cmake/macosx-legacy.cmake): the time.h of
 * clang-retro MFC (port/compat/macosx) and of the SDK, and the
 * reentrant functions of POSIX.
 *
 * The time.h of the 10.4 SDK does not declare gmtime_r, localtime_r,
 * asctime_r and ctime_r when _POSIX_C_SOURCE is set. These functions are
 * POSIX functions, and libSystem of 10.4 has them. Mbed TLS sets
 * _POSIX_C_SOURCE and calls gmtime_r.
 */
#ifndef DMODEM_MACOSX_LEGACY_TIME_H
#define DMODEM_MACOSX_LEGACY_TIME_H

#include_next <time.h>

#if defined(_POSIX_C_SOURCE) && !defined(_ANSI_SOURCE)
#ifdef __cplusplus
extern "C" {
#endif
/* Converts a time to UTC in the given structure. */
struct tm *gmtime_r(const time_t *, struct tm *);
/* Converts a time to local time in the given structure. */
struct tm *localtime_r(const time_t *, struct tm *);
/* Writes a time structure as text into the given buffer. */
char *asctime_r(const struct tm *, char *);
/* Writes a time as text into the given buffer. */
char *ctime_r(const time_t *, char *);
#ifdef __cplusplus
}
#endif
#endif

#endif
