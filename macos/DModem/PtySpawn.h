/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * PtySpawn.h - a program started on a new pty (the Remote page's ssh):
 *              Swift can't fork, so this is C
 */
#ifndef DMODEM_PTY_SPAWN_H
#define DMODEM_PTY_SPAWN_H

#include <sys/types.h>

/* path started with argv (NULL-ended) and TERM=term on a pty of cols by
   rows, as the leader of its own session: the pty's master (non-blocking),
   and its pid in *pid; -1 if it could not be */
int dm_pty_spawn(const char *path, const char *const *argv, const char *term,
		 int cols, int rows, pid_t *pid);
/* the pty's new size, told to the program (SIGWINCH) */
void dm_pty_resize(int fd, int cols, int rows);

#endif
