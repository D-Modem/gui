/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * PtySpawn.c - a program started on a new pty, for the Remote page's ssh
 */
#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <util.h>

#include "PtySpawn.h"

int dm_pty_spawn(const char *path, const char *const *argv, const char *term,
		 int cols, int rows, pid_t *pid)
{
	struct winsize ws = { 0 };
	ws.ws_col = (unsigned short)cols;
	ws.ws_row = (unsigned short)rows;
	int fd;
	pid_t p = forkpty(&fd, NULL, NULL, &ws);
	if (p < 0)
		return -1;
	if (p == 0) {
		setenv("TERM", term, 1);
		execv(path, (char *const *)argv);
		_exit(127);
	}
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	*pid = p;
	return fd;
}

void dm_pty_resize(int fd, int cols, int rows)
{
	struct winsize ws = { 0 };
	ws.ws_col = (unsigned short)cols;
	ws.ws_row = (unsigned short)rows;
	ioctl(fd, TIOCSWINSZ, &ws);
}
