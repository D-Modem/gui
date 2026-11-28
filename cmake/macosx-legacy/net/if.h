/*
 * net/if.h for Mac OS X 10.4 (cmake/macosx-legacy.cmake): sys/socket.h,
 * then the net/if.h of the SDK.
 *
 * The net/if.h of the 10.4 SDK uses struct sockaddr and does not include
 * sys/socket.h. POSIX lets a program include net/if.h first, and libssh
 * does.
 */
#ifndef DMODEM_MACOSX_LEGACY_NET_IF_H
#define DMODEM_MACOSX_LEGACY_NET_IF_H

#include <sys/types.h>
#include <sys/socket.h>
#include_next <net/if.h>

#endif
