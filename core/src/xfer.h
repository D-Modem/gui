/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * xfer.h - the file transfers' parts (xfer*.c): a transfer runs on a
 *          thread of its own, its line two queues (what came from the
 *          remote, what goes to it) that the app's thread fills and
 *          drains; a failure or a cancel goes back to the thread's start
 *          with longjmp, the files it had open closed there.
 */
#ifndef DMC_XFER_H
#define DMC_XFER_H

#include <setjmp.h>
#include <stdio.h>

#include "dmodemctl.h"
#include "util.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

#define XFER_PATH 1024
#define XFER_NAME 256
/* what may wait to go to the remote before the protocol's thread waits */
#define XFER_OUT_MAX (32 * 1024)

/* a file to send */
struct xfer_file {
	char *path;
	char name[XFER_NAME];
	long long size;                 /* -1 not known */
	long long mtime;                /* seconds, 0 not known */
};

struct dmc_xfer {
	int protocol, sending;
	char folder[XFER_PATH];         /* received files' */
	char xname[XFER_NAME];          /* XMODEM's (it sends no name) */
	struct xfer_file *files;
	int nfiles;

	/* the line, and the status: the app's thread and the protocol's */
#ifdef _WIN32
	CRITICAL_SECTION cs;
	HANDLE ev_in, ev_out, thread;
#else
	pthread_mutex_t mu;
	pthread_cond_t cv_in, cv_out;
	pthread_t thread;
#endif
	int started;
	void (*notify)(void *);
	void *notify_ctx;
	int notified;
	dmc_buf in, out;
	volatile int cancelled;
	dmc_xfer_status st;
	char **names;
	int nnames;

	/* the protocol's thread's alone */
	unsigned char rbuf[4096];
	int rpos, rlen;
	jmp_buf jb;
	char why[256];
	FILE *src;                      /* the file being sent */
	FILE *dst;                      /* the file being received */
	char dst_path[XFER_PATH], dst_name[XFER_NAME + 16];
	int keep_on_cancel;             /* plain text: what came is kept */
	int keep_partial;               /* crash recovery: a part is kept */
	dmc_xfer_options opt;
	volatile int skip;              /* the user skipped the file now */
	void *heap;                     /* freed at the end */
	void *proto;                    /* the protocol's state (Kermit's) */
	void (*proto_free)(void *);     /* how it's freed (NULL: free) */
};

/* plain text's quiet that ends a receive (the tests' shorter) */
extern int xfer_text_quiet_ms;

/* the time in ms (monotonic) and a pause */
long long xfer_now(void);
void xfer_sleep(int ms);

/* a byte from the remote (0-255), -1 when none came within ms; a cancel
   jumps out */
int xfer_read(dmc_xfer *x, int ms);
/* the same, -2 for a cancel instead of the jump */
int xfer_read_nc(dmc_xfer *x, int ms);
/* the next byte left waiting, -1 for none: no wait */
int xfer_peek(dmc_xfer *x);
int xfer_available(dmc_xfer *x);
/* what is waiting dropped */
void xfer_purge(dmc_xfer *x);
/* bytes to the remote (a cancel jumps out) */
void xfer_write(dmc_xfer *x, const void *data, int n);
void xfer_put(dmc_xfer *x, int c);
int xfer_is_cancelled(dmc_xfer *x);

/* the transfer failed (why) or was cancelled: back to the start */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn, format(printf, 2, 3)))
#endif
void xfer_fail(dmc_xfer *x, const char *fmt, ...);
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void xfer_cancelled(dmc_xfer *x, const char *why);

/* the status as it goes */
void xfer_file_start(dmc_xfer *x, const char *name, long long size);
void xfer_bytes(dmc_xfer *x, long long bytes);
void xfer_size(dmc_xfer *x, long long size);
void xfer_file_done(dmc_xfer *x);
void xfer_error(dmc_xfer *x);
long long xfer_size_now(dmc_xfer *x);

/* a received file made in the folder (its name made safe and unique):
   x->dst open, x->dst_name the name it got */
void xfer_create(dmc_xfer *x, const char *name);
/* the same for crash recovery: a part of the file in the folder
   continued (its length, appended to), or a new one (0); -2 when the
   file is there whole (size known), and nothing opened */
long long xfer_create_resume(dmc_xfer *x, const char *name, long long size);
/* the user's skip of the file now, taken (once) */
int xfer_skip_wanted(dmc_xfer *x);
/* the received file written */
void xfer_dst_write(dmc_xfer *x, const void *data, int n);
/* the received file whole (its time set, if mtime > 0), or dropped */
void xfer_done(dmc_xfer *x, long long mtime);
void xfer_abandon(dmc_xfer *x);
/* a file to send opened (x->src), and closed */
void xfer_open(dmc_xfer *x, const struct xfer_file *f);
void xfer_close(dmc_xfer *x);
/* up to n bytes of x->src: how many (0 at its end) */
int xfer_src_read(dmc_xfer *x, void *buf, int n);

/* the checks: CRC-16/XMODEM, and CRC-32 kept inverted (ZMODEM's) */
unsigned xfer_crc16(unsigned crc, const unsigned char *p, int n);
unsigned xfer_crc16_byte(unsigned crc, int b);
unsigned long xfer_crc32(unsigned long crc, const unsigned char *p, int n);
unsigned long xfer_crc32_byte(unsigned long crc, int b);

/* the protocols (xfer_xy.c, xfer_z.c, xfer_kermit.c, xfer_hex.c) */
void xy_receive_x(dmc_xfer *x);
void xy_receive_y(dmc_xfer *x, int g);
void xy_send_x(dmc_xfer *x, int one_k);
void xy_send_y(dmc_xfer *x, int g);
void z_receive(dmc_xfer *x);
void z_send(dmc_xfer *x);
void kermit_receive(dmc_xfer *x);
void kermit_send(dmc_xfer *x);
/* Kermit's E packet that ends it, for a cancel: its length */
int kermit_cancel_bytes(dmc_xfer *x, char *out, int max);
void hex_receive(dmc_xfer *x, int srec);
void hex_send(dmc_xfer *x, int srec);
void text_receive(dmc_xfer *x);
void text_send(dmc_xfer *x);

/* ZMODEM's starts, and what cancels it */
extern const char z_sz_start[6], z_rz_start[6];
extern const char z_cancel[20];

#endif
