/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * xfer_xy.c - XMODEM (128-byte blocks, a checksum or CRC; 1K blocks) and
 *             YMODEM (1K blocks, CRC, a batch of files each named and
 *             sized in block 0), both ways.  A bad block is asked for
 *             again (NAK) up to MAX_ERRORS times; CAN CAN from the other
 *             side ends it.
 */
#include <stdlib.h>
#include <string.h>

#include "xfer.h"

#define SOH 0x01
#define STX 0x02
#define EOT 0x04
#define ACK 0x06
#define NAK 0x15
#define CAN 0x18
#define SUB 0x1a
#define C 'C'
#define MAX_ERRORS 10
#define TIMEOUT 10000

/* a block as it came */
struct block {
	int number;                     /* -1: none (a timeout, a bad one, EOT) */
	int size;
	unsigned char data[1024];
};

/* the receiver's state */
struct rx {
	int crc;                        /* CRC, else the checksum */
	int eot;                        /* the sender's EOT came */
	int bad;                        /* a damaged block came */
};

static int sum8(const unsigned char *p, int n)
{
	int s = 0;
	for (int i = 0; i < n; i++)
		s += p[i];
	return s & 0xff;
}

/* a block within wait ms, its check checked; number -1 for a timeout, a
   bad block (the rest of it let go by) or EOT (r->eot) */
static void read_block(dmc_xfer *x, struct rx *r, struct block *b, int use_crc,
		       int wait)
{
	r->eot = r->bad = 0;
	b->number = -1;
	/* (what isn't a block's start let go by: messages, line noise) */
	long long deadline = xfer_now() + wait;
	int size = 0;
	while (!size) {
		long long left = deadline - xfer_now();
		if (left <= 0)
			return;
		int c = xfer_read(x, (int)left);
		switch (c) {
		case -1: return;
		case SOH: size = 128; break;
		case STX: size = 1024; break;
		case EOT: r->eot = 1; return;
		case CAN:
			if (xfer_read(x, 1000) == CAN)
				xfer_cancelled(x, "cancelled by the sender");
			break;
		}
	}
	int n = xfer_read(x, 1000), nn = xfer_read(x, 1000);
	if (n < 0 || nn < 0)
		return;
	for (int i = 0; i < size; i++) {
		int c = xfer_read(x, 1000);
		if (c < 0)
			return;
		b->data[i] = (unsigned char)c;
	}
	int ok;
	if (use_crc) {
		int hi = xfer_read(x, 1000), lo = xfer_read(x, 1000);
		ok = hi >= 0 && lo >= 0 &&
			xfer_crc16(0, b->data, size) == (unsigned)((hi << 8) | lo);
	} else {
		int s = xfer_read(x, 1000);
		ok = s >= 0 && sum8(b->data, size) == s;
	}
	if (!ok || (n ^ nn) != 0xff) {
		while (xfer_read(x, 200) >= 0)
			;
		r->bad = 1;
		return;
	}
	b->number = n;
	b->size = size;
}

/* the first block, asked for with C (CRC) and, for XMODEM, NAK (a
   checksum) when C isn't answered */
static void start_and_read(dmc_xfer *x, struct rx *r, struct block *b, int ymodem)
{
	int crc = 1;
	r->bad = 0;
	for (int attempt = 0; attempt < 10; attempt++) {
		if (attempt == 4 && !ymodem)
			crc = 0;
		/* (a damaged block 0 asked for again with NAK: a C could be
		   taken for its ACK and a request for the data) */
		xfer_put(x, crc && !(ymodem && r->bad) ? C : NAK);
		read_block(x, r, b, crc, 3000);
		if (b->number >= 0) {
			r->crc = crc;
			return;
		}
	}
	b->number = -1;
}

/* a file's blocks into the received file, up to size bytes if known;
   held (XMODEM): the last block kept back, for its padding to come off */
static void receive_file(dmc_xfer *x, struct rx *r, long long size, int ymodem,
			 unsigned char *held, int *held_n)
{
	int expect = 1, errors = 0, eots = 0, have = 0;
	long long written = 0;
	struct block bb, *b = &bb;
	if (held)
		*held_n = -1;
	if (!ymodem) {
		start_and_read(x, r, b, 0);
		have = b->number >= 0;
	}
	for (;;) {
		if (!have) {
			read_block(x, r, b, r->crc, TIMEOUT);
			if (b->number < 0) {
				if (r->eot) {
					/* YMODEM: the first EOT answered NAK, the
					   second ACK (to be sure it was one) */
					eots++;
					if (ymodem && eots == 1) {
						xfer_put(x, NAK);
						continue;
					}
					xfer_put(x, ACK);
					break;
				}
				if (++errors > MAX_ERRORS)
					xfer_fail(x, "too many errors on the line");
				xfer_error(x);
				xfer_put(x, NAK);
				continue;
			}
		}
		have = 0;
		if (b->number == (expect & 0xff)) {
			int len = b->size;
			if (size >= 0 && written + len > size)
				len = size - written > 0 ? (int)(size - written) : 0;
			if (held) {
				if (*held_n >= 0)
					xfer_dst_write(x, held, *held_n);
				memcpy(held, b->data, (size_t)len);
				*held_n = len;
			} else {
				xfer_dst_write(x, b->data, len);
			}
			written += len;
			expect++;
			errors = 0;
			xfer_put(x, ACK);
			xfer_bytes(x, written);
		} else if (b->number == ((expect - 1) & 0xff)) {
			/* a repeat of one already had (our ACK lost) */
			xfer_put(x, ACK);
		} else {
			xfer_write(x, "\x18\x18\x18", 3);
			xfer_fail(x, "blocks out of order");
		}
	}
}

void xy_receive_x(dmc_xfer *x)
{
	struct rx r = { 1, 0, 0 };
	unsigned char held[1024];
	int held_n = -1;
	xfer_create(x, x->xname);
	xfer_file_start(x, x->dst_name, -1);
	receive_file(x, &r, -1, 0, held, &held_n);
	/* the last block, its padding off */
	if (held_n > 0) {
		while (held_n > 0 && held[held_n - 1] == SUB)
			held_n--;
		xfer_dst_write(x, held, held_n);
	}
	xfer_done(x, 0);
	xfer_file_done(x);
}

void xy_receive_y(dmc_xfer *x)
{
	struct rx r = { 1, 0, 0 };
	struct block bb0, *b0 = &bb0;
	for (;;) {
		/* block 0: the file's name and size, or an empty one (the end) */
		start_and_read(x, &r, b0, 1);
		if (b0->number < 0)
			xfer_fail(x, "the sender didn't start");
		if (b0->number != 0)
			xfer_fail(x, "no YMODEM header block");
		if (b0->data[0] == 0) {
			xfer_put(x, ACK);
			return;
		}
		char name[XFER_NAME], meta[1100];
		int nul = 0;
		while (nul < b0->size && b0->data[nul])
			nul++;
		snprintf(name, sizeof(name), "%.*s", nul, (const char *)b0->data);
		int m = 0;
		for (int i = nul + 1; i < b0->size && b0->data[i] && m < (int)sizeof(meta) - 1; i++)
			meta[m++] = (char)b0->data[i];
		meta[m] = '\0';
		long long size = -1, mtime = 0;
		char *end;
		long long v = strtoll(meta, &end, 10);
		if (end != meta)
			size = v;
		if (*end == ' ')
			mtime = strtoll(end + 1, NULL, 8);
		xfer_put(x, ACK);
		xfer_create(x, name);
		xfer_file_start(x, x->dst_name, size);
		xfer_put(x, C);
		receive_file(x, &r, size, 1, NULL, NULL);
		xfer_done(x, mtime);
		xfer_file_done(x);
	}
}

/* ------------------------------------------------------------------ */
/* sending */

/* C (CRC: 1) or NAK (a checksum: 0) from the receiver; -1 for neither */
static int wait_for_start(dmc_xfer *x)
{
	long long end = xfer_now() + 60000;
	while (xfer_now() < end) {
		switch (xfer_read(x, TIMEOUT)) {
		case C: return 1;
		case NAK: return 0;
		case CAN:
			if (xfer_read(x, 1000) == CAN)
				xfer_cancelled(x, "cancelled by the receiver");
			break;
		}
	}
	return -1;
}

/* the receiver's answer: ACK, or NAK (also for a timeout); C an ACK for
   block 0 when allow_c (receivers that ask for the data at once) */
static int answer(dmc_xfer *x, int allow_c, int wait)
{
	long long deadline = xfer_now() + wait;
	for (;;) {
		long long left = deadline - xfer_now();
		if (left <= 0)
			return NAK;
		switch (xfer_read(x, (int)left)) {
		case -1: return NAK;
		case ACK: return ACK;
		case NAK: return NAK;
		case C:
			if (allow_c)
				return ACK;
			break;
		case CAN:
			if (xfer_read(x, 1000) == CAN)
				xfer_cancelled(x, "cancelled by the receiver");
			break;
		}
	}
}

/* block n's bytes into out: SOH or STX, its number and complement, data,
   its CRC-16 or checksum: their length */
static int block_bytes(unsigned char *out, int n, const unsigned char *data,
		       int size, int use_crc)
{
	int k = 0;
	out[k++] = size == 1024 ? STX : SOH;
	out[k++] = (unsigned char)(n & 0xff);
	out[k++] = (unsigned char)(0xff - (n & 0xff));
	memcpy(out + k, data, (size_t)size);
	k += size;
	if (use_crc) {
		unsigned c = xfer_crc16(0, data, size);
		out[k++] = (unsigned char)(c >> 8);
		out[k++] = (unsigned char)(c & 0xff);
	} else {
		out[k++] = (unsigned char)sum8(data, size);
	}
	return k;
}

/* a block sent until it's acknowledged */
static void send_block(dmc_xfer *x, int n, const unsigned char *data, int size,
		       int use_crc)
{
	unsigned char bytes[1024 + 5];
	int len = block_bytes(bytes, n, data, size, use_crc);
	int errors = 0;
	for (;;) {
		xfer_write(x, bytes, len);
		if (answer(x, n == 0, TIMEOUT) == ACK)
			return;
		if (++errors > MAX_ERRORS)
			xfer_fail(x, "too many errors on the line");
		xfer_error(x);
	}
}

/* the open file's data in blocks of block_size (a short last one in 128,
   padded with SUB), then EOT until acknowledged */
static void send_data(dmc_xfer *x, int block_size, int use_crc)
{
	unsigned char buf[1024];
	int n = 1;
	long long sent = 0;
	for (;;) {
		int len = 0;
		while (len < block_size) {
			int r = xfer_src_read(x, buf + len, block_size - len);
			if (r <= 0)
				break;
			len += r;
		}
		if (!len)
			break;
		int size = len <= 128 ? 128 : block_size;
		memset(buf + len, SUB, (size_t)(size - len));
		send_block(x, n, buf, size, use_crc);
		sent += len;
		n++;
		xfer_bytes(x, sent);
		if (len < block_size)
			break;
	}
	/* the end: EOT until it's answered ACK (YMODEM answers the first NAK) */
	for (int tries = 0;; tries++) {
		xfer_put(x, EOT);
		if (answer(x, 0, TIMEOUT) == ACK)
			return;
		if (tries >= MAX_ERRORS)
			xfer_fail(x, "the end not acknowledged");
	}
}

void xy_send_x(dmc_xfer *x, int one_k)
{
	const struct xfer_file *f = &x->files[0];
	int use_crc = wait_for_start(x);
	if (use_crc < 0)
		xfer_fail(x, "the receiver didn't start");
	xfer_file_start(x, f->name, f->size);
	xfer_open(x, f);
	send_data(x, one_k ? 1024 : 128, use_crc);
	xfer_close(x);
	xfer_file_done(x);
}

void xy_send_y(dmc_xfer *x)
{
	unsigned char block0[1024];
	for (int i = 0; i < x->nfiles; i++) {
		const struct xfer_file *f = &x->files[i];
		if (wait_for_start(x) != 1)
			xfer_fail(x, "the receiver didn't start");
		memset(block0, 0, sizeof(block0));
		int nl = (int)strlen(f->name);
		if (nl > 900)
			nl = 900;
		memcpy(block0, f->name, (size_t)nl);
		char meta[64];
		int ml = snprintf(meta, sizeof(meta), "%lld %llo 0",
				  f->size >= 0 ? f->size : 0LL,
				  (unsigned long long)(f->mtime > 0 ? f->mtime : 0));
		memcpy(block0 + nl + 1, meta, (size_t)ml);
		send_block(x, 0, block0, nl + 1 + ml > 128 ? 1024 : 128, 1);
		xfer_file_start(x, f->name, f->size);
		if (wait_for_start(x) != 1)
			xfer_fail(x, "the receiver didn't ask for the data");
		xfer_open(x, f);
		send_data(x, 1024, 1);
		xfer_close(x);
		xfer_file_done(x);
	}
	/* the batch over: an empty block 0 */
	if (wait_for_start(x) != 1)
		xfer_fail(x, "the receiver didn't ask for the end");
	/* (rb exits as it takes it: its ACK can be lost with it) */
	unsigned char empty[128], bytes[133];
	memset(empty, 0, sizeof(empty));
	int len = block_bytes(bytes, 0, empty, 128, 1);
	for (int attempt = 0; attempt < 3; attempt++) {
		xfer_write(x, bytes, len);
		if (answer(x, 1, 2000) == ACK)
			return;
	}
}
