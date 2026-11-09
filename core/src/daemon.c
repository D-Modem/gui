/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * daemon.c - reading what dmodemd prints as it starts (dmodemctl.h)
 *
 * On Linux, macOS, BSD and Haiku it says
 *     Use `/dev/ttyDM0' as modem device, Ctrl+C for termination.
 * (the pty's own path, /dev/pts/3, when it may not make the link), and
 * on Windows
 *     Use TCP 127.0.0.1:2323 as the modem, Ctrl+C for termination.
 */
#include <string.h>

#include "dmodemctl.h"

static int copy(const char *from, size_t n, char *buf, int max)
{
	if (!buf || max <= 0)
		return 1;
	if (n >= (size_t)max)
		n = (size_t)max - 1;
	memcpy(buf, from, n);
	buf[n] = '\0';
	return 1;
}

int dmc_daemon_ready(const char *line, char *buf, int max)
{
	if (!line)
		return 0;
	const char *p = strstr(line, "Use `");
	if (p) {
		p += 5;
		const char *e = strchr(p, '\'');
		if (e && strstr(e, "as modem device"))
			return copy(p, (size_t)(e - p), buf, max);
	}
	p = strstr(line, "Use ");
	if (p) {
		p += 4;
		const char *e = strstr(p, " as the modem");
		if (e)
			return copy(p, (size_t)(e - p), buf, max);
	}
	return 0;
}

int dmc_daemon_error(const char *line)
{
	return line && !strncmp(line, "error:", 6);
}
