/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * direct.h - for libssh on picolibc (the Windows 95 build of clang-retro),
 *            which has no <direct.h>: the names of Microsoft's C library
 *            that libssh's misc.c uses, as picolibc's POSIX functions.
 */
#ifndef DMODEM_PICOLIBC_DIRECT_H
#define DMODEM_PICOLIBC_DIRECT_H
#include <unistd.h>
#include <sys/stat.h>
#define _mkdir(path) mkdir((path), 0700)
#define _access access
#define _stat stat
#define _S_IFDIR S_IFDIR
#define _S_IWRITE S_IWUSR
#endif
