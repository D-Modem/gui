/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * gai_strerror.c - gai_strerrorA for the Windows 9x builds (clang-retro:
 *                  picolibc, win98 and winme).  The one of mingw-w64's libws2_32.a calls
 *                  FormatMessageW, which Windows 9x has only as a stub.
 *                  This one is linked before it (ssh/CMakeLists.txt) and
 *                  calls FormatMessageA.  (Only Windows: the macOS
 *                  project compiles each file of ssh/src.)
 */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>

char *gai_strerrorA(int ecode)
{
	/* (one buffer, as mingw-w64's: libssh copies the text at once) */
	static char text[256];
	DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM |
				 FORMAT_MESSAGE_IGNORE_INSERTS |
				 FORMAT_MESSAGE_MAX_WIDTH_MASK,
				 NULL, (DWORD)ecode,
				 MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
				 text, sizeof text, NULL);

	if (n == 0)
		snprintf(text, sizeof text, "name lookup error %d", ecode);
	return text;
}
#else
typedef int dmodem_gai_strerror_unused; /* (ISO C: no empty file) */
#endif
