/*
 * signal.h for Mac OS X 10.4 (cmake/macosx-legacy.cmake): the signal.h
 * of the SDK, with SIG_DFL, SIG_IGN, SIG_HOLD and SIG_ERR as pointers to
 * a function with a prototype.
 *
 * Without _POSIX_C_SOURCE, the 10.4 SDK gives them as pointers to a
 * function with no prototype. libssh compiles with
 * -Werror=strict-prototypes, which stops at these.
 */
#ifndef DMODEM_MACOSX_LEGACY_SIGNAL_H
#define DMODEM_MACOSX_LEGACY_SIGNAL_H

#include_next <signal.h>

#undef SIG_DFL
#undef SIG_IGN
#undef SIG_HOLD
#undef SIG_ERR
#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SIG_HOLD ((void (*)(int))5)
#define SIG_ERR ((void (*)(int))-1)

#endif
