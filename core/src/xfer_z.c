/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * xfer_z.c - ZMODEM (Forsberg's, as lrzsz's sz and rz speak it), both
 *            ways: hex and binary headers (CRC-16, and CRC-32 when both
 *            sides can), ZDLE escapes, data in subpackets streamed
 *            (ZCRCG) with the receiver's ZRPOS putting the sender back
 *            where the line went wrong, files offered by name, size and
 *            time, and ended with ZFIN and "OO".
 */
#include <stdlib.h>
#include <string.h>

#include "xfer.h"

#define ZPAD '*'
#define ZDLE 0x18
#define CAN 0x18
#define XON 0x11
#define XOFF 0x13

enum {
	ZRQINIT, ZRINIT, ZSINIT, ZACK, ZFILE, ZSKIP, ZNAK, ZABORT, ZFIN,
	ZRPOS, ZDATA, ZEOF, ZFERR, ZCRC, ZCHALLENGE, ZCOMPL, ZCAN, ZFREECNT,
	ZCOMMAND
};

#define ZCRCE 'h'
#define ZCRCG 'i'
#define ZCRCQ 'j'
#define ZCRCW 'k'
#define GOT_END 0x100
#define BAD 0x200
#define QUIET -1

#define CANFDX 0x01
#define CANOVIO 0x02
#define CANFC32 0x20
#define ZCBIN 1
#define ZCRECOV 3                       /* ZF0: continue a part of the file */

#define BLOCK 1024
/* bytes between the sender's ZCRCQs (its window) */
#define WINDOW (16 * 1024)
#define MAX_ERRORS 10
#define TIMEOUT 10000
/* after a ZRPOS, the time the sender's stream from before it may still
   take to stop */
#define RPOS_GRACE_MS 1000

const char z_sz_start[6] = { ZPAD, ZPAD, ZDLE, 'B', '0', '0' };
const char z_rz_start[6] = { ZPAD, ZPAD, ZDLE, 'B', '0', '1' };
/* what stops a transfer at the other end: CANs, then backspaces */
const char z_cancel[20] = {
	CAN, CAN, CAN, CAN, CAN, CAN, CAN, CAN, CAN, CAN,
	8, 8, 8, 8, 8, 8, 8, 8, 8, 8
};

struct header {
	int type;
	unsigned char p[4];             /* ZP0..ZP3; ZF0 is p[3] */
};

/* how readHeader went */
enum { H_OK, H_BAD, H_QUIET, H_NONE };

struct z {
	dmc_xfer *x;
	int crc32;                      /* the receiver takes CRC-32 */
	int rx_crc32;                   /* the sender's data comes with it */
	int last_sent;
	unsigned char data[8200];       /* a data subpacket that came */
	unsigned char out[2 * 8200 + 32];
	int on;
};

static long long hpos(const struct header *h)
{
	return (long long)h->p[0] | ((long long)h->p[1] << 8) |
		((long long)h->p[2] << 16) | ((long long)h->p[3] << 24);
}

static struct header hdr_pos(int type, long long pos)
{
	struct header h;
	h.type = type;
	for (int i = 0; i < 4; i++)
		h.p[i] = (unsigned char)((pos >> (8 * i)) & 0xff);
	return h;
}

static struct header hdr_flags(int type, int f0)
{
	struct header h;
	h.type = type;
	h.p[0] = h.p[1] = h.p[2] = 0;
	h.p[3] = (unsigned char)f0;
	return h;
}

/* ------------------------------------------------------------------ */
/* sending frames */

static void out_c(struct z *z, int c)
{
	if (z->on < (int)sizeof(z->out))
		z->out[z->on++] = (unsigned char)c;
}

static void flush(struct z *z)
{
	xfer_write(z->x, z->out, z->on);
	z->on = 0;
}

static void hex2(struct z *z, int v)
{
	static const char d[] = "0123456789abcdef";
	out_c(z, d[(v >> 4) & 15]);
	out_c(z, d[v & 15]);
}

/* ZDLE escapes: ZDLE, XON/XOFF, DLE (and their high-bit forms), and CR
   after @ (a Telnet escape) */
static void escape(struct z *z, int c)
{
	int esc;
	switch (c) {
	case ZDLE: case 0x10: case 0x90: case 0x11: case 0x91: case 0x13: case 0x93:
		esc = 1;
		break;
	case 0x0d: case 0x8d:
		esc = (z->last_sent & 0x7f) == '@';
		break;
	default:
		esc = 0;
	}
	if (esc) {
		out_c(z, ZDLE);
		out_c(z, c ^ 0x40);
	} else {
		out_c(z, c);
	}
	z->last_sent = c;
}

/* a header in hex (the first ones, and the receiver's) */
static void send_hex(struct z *z, struct header h)
{
	unsigned char raw[5];
	raw[0] = (unsigned char)h.type;
	memcpy(raw + 1, h.p, 4);
	out_c(z, ZPAD);
	out_c(z, ZPAD);
	out_c(z, ZDLE);
	out_c(z, 'B');
	for (int i = 0; i < 5; i++)
		hex2(z, raw[i]);
	unsigned crc = xfer_crc16(0, raw, 5);
	hex2(z, (int)(crc >> 8));
	hex2(z, (int)(crc & 0xff));
	out_c(z, 0x0d);
	out_c(z, 0x8a);
	/* (an XON after all but ZACK and ZFIN, as sz does) */
	if (h.type != ZACK && h.type != ZFIN)
		out_c(z, XON);
	flush(z);
}

/* a header in binary (the sender's): CRC-32 when the receiver can */
static void send_bin(struct z *z, struct header h)
{
	unsigned char raw[5];
	raw[0] = (unsigned char)h.type;
	memcpy(raw + 1, h.p, 4);
	out_c(z, ZPAD);
	out_c(z, ZDLE);
	out_c(z, z->crc32 ? 'C' : 'A');
	for (int i = 0; i < 5; i++)
		escape(z, raw[i]);
	if (z->crc32) {
		unsigned long c = ~xfer_crc32(0xffffffffUL, raw, 5) & 0xffffffffUL;
		for (int i = 0; i < 4; i++)
			escape(z, (int)((c >> (8 * i)) & 0xff));
	} else {
		unsigned c = xfer_crc16(0, raw, 5);
		escape(z, (int)(c >> 8));
		escape(z, (int)(c & 0xff));
	}
	flush(z);
}

/* a data subpacket ended by end (ZCRCE, ZCRCG, ZCRCQ, ZCRCW) */
static void send_data(struct z *z, const unsigned char *data, int len, int end)
{
	for (int i = 0; i < len; i++)
		escape(z, data[i]);
	out_c(z, ZDLE);
	out_c(z, end);
	if (z->crc32) {
		unsigned long c = xfer_crc32(0xffffffffUL, data, len);
		c = ~xfer_crc32_byte(c, end) & 0xffffffffUL;
		for (int i = 0; i < 4; i++)
			escape(z, (int)((c >> (8 * i)) & 0xff));
	} else {
		unsigned c = xfer_crc16_byte(xfer_crc16(0, data, len), end);
		escape(z, (int)(c >> 8));
		escape(z, (int)(c & 0xff));
	}
	if (end == ZCRCW)
		out_c(z, XON);
	flush(z);
}

/* ------------------------------------------------------------------ */
/* reading frames */

static int is_flow(int c)
{
	return c == XON || c == XOFF || c == (XON | 0x80) || c == (XOFF | 0x80);
}

/* a byte with ZDLE's escapes undone; GOT_END|ZCRCx for a frame's end,
   BAD for a bad escape, QUIET when none came in time */
static int read_escaped(struct z *z)
{
	for (;;) {
		int c = xfer_read(z->x, TIMEOUT);
		if (c < 0)
			return QUIET;
		if (is_flow(c))
			continue;
		if (c != ZDLE)
			return c;
		int cans = 1;
		for (;;) {
			int d = xfer_read(z->x, TIMEOUT);
			if (d < 0)
				return QUIET;
			if (is_flow(d))
				continue;
			if (d == CAN) {
				if (++cans >= 5)
					xfer_cancelled(z->x, "cancelled by the other side");
				continue;
			}
			if (d == ZCRCE || d == ZCRCG || d == ZCRCQ || d == ZCRCW)
				return d | GOT_END;
			if (d == 'l')
				return 0x7f;
			if (d == 'm')
				return 0xff;
			if ((d & 0x60) == 0x40)
				return d ^ 0x40;
			return BAD;
		}
	}
}

static int hex_digit(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* a binary header's type, ZP0..ZP3 and check (CRC-32 if c32) */
static int read_bin_header(struct z *z, int c32, struct header *h)
{
	unsigned char raw[5];
	for (int i = 0; i < 5; i++) {
		int c = read_escaped(z);
		if (c == QUIET)
			return H_QUIET;
		if (c > 0xff)
			return H_BAD;
		raw[i] = (unsigned char)c;
	}
	if (c32) {
		unsigned long crc = 0;
		for (int i = 0; i < 4; i++) {
			int c = read_escaped(z);
			if (c == QUIET)
				return H_QUIET;
			if (c > 0xff)
				return H_BAD;
			crc |= (unsigned long)c << (8 * i);
		}
		if ((~xfer_crc32(0xffffffffUL, raw, 5) & 0xffffffffUL) != crc)
			return H_BAD;
	} else {
		int hi = read_escaped(z);
		if (hi == QUIET)
			return H_QUIET;
		int lo = read_escaped(z);
		if (lo == QUIET)
			return H_QUIET;
		if (hi > 0xff || lo > 0xff ||
		    xfer_crc16(0, raw, 5) != (unsigned)((hi << 8) | lo))
			return H_BAD;
	}
	z->rx_crc32 = c32;
	h->type = raw[0];
	memcpy(h->p, raw + 1, 4);
	return H_OK;
}

/* a hex header's seven octets and its CR LF */
static int read_hex_header(struct z *z, struct header *h)
{
	unsigned char raw[7];
	for (int i = 0; i < 7; i++) {
		int a = xfer_read(z->x, TIMEOUT);
		if (a < 0)
			return H_QUIET;
		int b = xfer_read(z->x, TIMEOUT);
		if (b < 0)
			return H_QUIET;
		int hi = hex_digit(a & 0x7f), lo = hex_digit(b & 0x7f);
		if (hi < 0 || lo < 0)
			return H_BAD;
		raw[i] = (unsigned char)((hi << 4) | lo);
	}
	if (xfer_crc16(0, raw, 5) != (unsigned)((raw[5] << 8) | raw[6]))
		return H_BAD;
	/* CR LF after every hex header (an XON after most: let go by with
	   the next header's garbage) */
	if (xfer_read(z->x, TIMEOUT) < 0 || xfer_read(z->x, TIMEOUT) < 0)
		return H_QUIET;
	h->type = raw[0];
	memcpy(h->p, raw + 1, 4);
	return H_OK;
}

/* the next header, what isn't one skipped: H_BAD when one came damaged
   (or garbage_max octets came without one and null_on_garbage), H_NONE
   for the garbage otherwise, H_QUIET when the line went quiet */
static int read_header(struct z *z, int garbage_max, int null_on_garbage,
		       struct header *h)
{
	int garbage = 0, cans = 0;
	for (;;) {
		int c = xfer_read(z->x, TIMEOUT);
		if (c < 0)
			return H_QUIET;
		if (c == CAN) {
			if (++cans >= 5)
				xfer_cancelled(z->x, "cancelled by the other side");
		} else {
			cans = 0;
		}
		if (c != ZDLE && c != (ZDLE | 0x80)) {
			if (++garbage > garbage_max)
				return null_on_garbage ? H_BAD : H_NONE;
			continue;
		}
		int k = xfer_read(z->x, TIMEOUT);
		if (k < 0)
			return H_QUIET;
		k &= 0x7f;
		/* (the ZDLE before a header is also a CAN: a cancel is CANs in
		   a row) */
		if (k == CAN) {
			if (++cans >= 5)
				xfer_cancelled(z->x, "cancelled by the other side");
			continue;
		}
		cans = 0;
		switch (k) {
		case 'A': return read_bin_header(z, 0, h);
		case 'C': return read_bin_header(z, 1, h);
		case 'B': return read_hex_header(z, h);
		}
	}
}

/* the next header, or 0 when it was damaged or didn't come */
static int header_or_null(struct z *z, struct header *h)
{
	return read_header(z, 64 * 1024, 0, h) == H_OK;
}

/* a data subpacket into z->data: its length (*end how it ended); -1 if
   its check failed or it ran over max; -2 when the line went quiet */
static int read_data(struct z *z, int max, int *end)
{
	int n = 0;
	for (;;) {
		int c = read_escaped(z);
		if (c == QUIET)
			return -2;
		if (c == BAD)
			return -1;
		if (c > 0xff) {
			int e = c & 0xff;
			*end = e;
			if (z->rx_crc32) {
				unsigned long crc = 0;
				for (int i = 0; i < 4; i++) {
					int v = read_escaped(z);
					if (v == QUIET)
						return -2;
					if (v > 0xff)
						return -1;
					crc |= (unsigned long)v << (8 * i);
				}
				unsigned long want = xfer_crc32(0xffffffffUL, z->data, n);
				want = ~xfer_crc32_byte(want, e) & 0xffffffffUL;
				if (want != crc)
					return -1;
			} else {
				int hi = read_escaped(z);
				if (hi == QUIET)
					return -2;
				int lo = read_escaped(z);
				if (lo == QUIET)
					return -2;
				if (hi > 0xff || lo > 0xff ||
				    xfer_crc16_byte(xfer_crc16(0, z->data, n), e) !=
				    (unsigned)((hi << 8) | lo))
					return -1;
			}
			return n;
		}
		if (n >= max)
			return -1;
		z->data[n++] = (unsigned char)c;
	}
}

/* read_data, the line's quiet a failure */
static int read_data_or_fail(struct z *z, int max, int *end)
{
	int n = read_data(z, max, end);
	if (n == -2)
		xfer_fail(z->x, "the other side went quiet");
	return n;
}

static struct z *z_new(dmc_xfer *x)
{
	struct z *z = (struct z *)calloc(1, sizeof(*z));
	if (!z)
		xfer_fail(x, "out of memory");
	z->x = x;
	x->proto = z;
	return z;
}

/* ------------------------------------------------------------------ */
/* receiving (the other side runs sz) */

static void zrinit(struct z *z)
{
	send_hex(z, hdr_flags(ZRINIT, CANFDX | CANOVIO | CANFC32));
}

/* a file's data after its ZFILE (its info, len bytes; f0 its ZF0): 1
   received, 0 skipped */
static int receive_file(struct z *z, const unsigned char *info, int len, int f0)
{
	dmc_xfer *x = z->x;
	char name[XFER_NAME], meta[256];
	int nul = 0;
	while (nul < len && info[nul])
		nul++;
	snprintf(name, sizeof(name), "%.*s", nul, (const char *)info);
	int m = 0;
	for (int i = nul + 1; i < len && info[i] && m < (int)sizeof(meta) - 1; i++)
		meta[m++] = (char)info[i];
	meta[m] = '\0';
	long long size = -1, mtime = 0;
	char *p = meta, *e;
	while (*p == ' ')
		p++;
	long long v = strtoll(p, &e, 10);
	if (e != p)
		size = v;
	if (*e == ' ')
		mtime = strtoll(e + 1, NULL, 8);
	long long pos = 0, rpos_at = 0;
	/* crash recovery (ours, or the sender's ZCRECOV): from the end of a
	   part that is in the folder */
	if (x->opt.resume || f0 == ZCRECOV) {
		pos = xfer_create_resume(x, name, size);
		if (pos == -2) {
			/* (there whole already) */
			send_hex(z, hdr_pos(ZSKIP, 0));
			return 0;
		}
	} else {
		xfer_create(x, name);
	}
	xfer_file_start(x, x->dst_name, size);
	xfer_bytes(x, pos);
	int errors = 0, ok = 0;
	struct header h;
	/* (until the sender has had time to hear our ZRPOS, what still comes
	   of its stream from before isn't an error, and isn't asked for
	   again: each ZRPOS a restart of its own) */
#define RPOS() (send_hex(z, hdr_pos(ZRPOS, pos)), rpos_at = xfer_now())
#define WAITING() (xfer_now() - rpos_at < RPOS_GRACE_MS)
	RPOS();
	for (;;) {
		/* the user's skip: ZSKIP, and the file dropped (the sender
		   goes on to its next) */
		if (xfer_skip_wanted(x)) {
			send_hex(z, hdr_pos(ZSKIP, 0));
			xfer_purge(x);
			break;
		}
		/* (8 KB of a stream with no header in it: ours lost; asked
		   for again) */
		int st = read_header(z, 8 * 1024, 1, &h);
		if (st == H_QUIET)
			xfer_fail(x, "the other side went quiet");
		if (st != H_OK) {
			if (WAITING())
				continue;
			if (++errors > MAX_ERRORS)
				xfer_fail(x, "too many errors on the line");
			RPOS();
			continue;
		}
		if (h.type == ZDATA) {
			if (hpos(&h) != pos) {
				/* not where we are: back to it (not an error) */
				RPOS();
				continue;
			}
			for (;;) {
				int end;
				int n = read_data_or_fail(z, 8192, &end);
				if (n < 0) {
					if (++errors > MAX_ERRORS)
						xfer_fail(x, "too many errors on the line");
					xfer_error(x);
					xfer_purge(x);
					RPOS();
					break;
				}
				xfer_dst_write(x, z->data, n);
				pos += n;
				errors = 0;
				xfer_bytes(x, pos);
				if (end == ZCRCW) {
					send_hex(z, hdr_pos(ZACK, pos));
					break;
				}
				if (end == ZCRCQ)
					send_hex(z, hdr_pos(ZACK, pos));
				else if (end == ZCRCE)
					break;
				if (x->skip)
					break;
			}
		} else if (h.type == ZEOF) {
			if (hpos(&h) != pos) {
				/* (a stream's end from before our ZRPOS, or our
				   ZRPOS lost) */
				if (!WAITING())
					RPOS();
				continue;
			}
			ok = 1;
			break;
		} else if (h.type == ZFILE) {
			/* our ZRPOS lost: the same file offered again */
			int end;
			read_data_or_fail(z, 4096, &end);
			RPOS();
		} else if (h.type == ZSKIP) {
			break;
		} else if (h.type == ZCAN || h.type == ZABORT) {
			xfer_cancelled(x, "cancelled by the sender");
		} else if (h.type == ZFIN) {
			xfer_fail(x, "the sender ended in the middle of a file");
		}
	}
#undef RPOS
#undef WAITING
	if (!ok) {
		xfer_abandon(x);
		return 0;
	}
	xfer_done(x, mtime);
	return 1;
}

void z_receive(dmc_xfer *x)
{
	struct z *z = z_new(x);
	unsigned char info[4100];
	int errors = 0;
	zrinit(z);
	long long last_header = xfer_now();
	for (;;) {
		struct header h;
		int st = read_header(z, 64 * 1024, 0, &h);
		if (st == H_QUIET || st == H_NONE) {
			/* quiet: ask again (the sender may not have heard us) */
			if (xfer_now() - last_header > 60000 || ++errors > MAX_ERRORS)
				xfer_fail(x, st == H_QUIET ? "the other side went quiet" :
					  "no ZMODEM header");
			zrinit(z);
			continue;
		}
		if (st == H_BAD) {
			if (++errors > MAX_ERRORS)
				xfer_fail(x, "too many errors on the line");
			zrinit(z);
			continue;
		}
		last_header = xfer_now();
		int end, n;
		switch (h.type) {
		case ZRQINIT:
			zrinit(z);
			break;
		case ZSINIT:
			read_data_or_fail(z, 1024, &end);
			send_hex(z, hdr_pos(ZACK, 0));
			break;
		case ZFILE:
			n = read_data_or_fail(z, 4096, &end);
			if (n < 0) {
				send_hex(z, hdr_pos(ZNAK, 0));
				break;
			}
			memcpy(info, z->data, (size_t)n);
			if (receive_file(z, info, n, h.p[3])) {
				xfer_file_done(x);
				zrinit(z);
			}
			break;
		case ZFIN:
			send_hex(z, hdr_pos(ZFIN, 0));
			/* "OO" (over and out) follows: read past it if it comes */
			for (int i = 0; i < 2; i++)
				if (xfer_read(x, 500) < 0)
					return;
			return;
		case ZCAN:
		case ZABORT:
			xfer_cancelled(x, "cancelled by the sender");
		case ZCOMMAND:
			/* commands are never run here */
			read_data_or_fail(z, 4096, &end);
			send_hex(z, hdr_pos(ZCOMPL, 0));
			break;
		case ZFREECNT:
			send_hex(z, hdr_pos(ZACK, 0));
			break;
		}
	}
}

/* ------------------------------------------------------------------ */
/* sending (the other side runs rz) */

/* ZRQINIT until the receiver's ZRINIT (a ZCHALLENGE answered on the way) */
static struct header wait_for_zrinit(struct z *z)
{
	int tries = 0;
	struct header h;
	send_hex(z, hdr_pos(ZRQINIT, 0));
	for (;;) {
		int st = read_header(z, 64 * 1024, 0, &h);
		if (st == H_QUIET || st == H_NONE) {
			if (++tries > MAX_ERRORS)
				xfer_fail(z->x, "the receiver didn't answer");
			send_hex(z, hdr_pos(ZRQINIT, 0));
			continue;
		}
		if (st == H_OK) {
			switch (h.type) {
			case ZRINIT:
				return h;
			case ZCHALLENGE: {
				struct header a = h;
				a.type = ZACK;
				send_hex(z, a);
				continue;
			}
			case ZCAN:
			case ZABORT:
				xfer_cancelled(z->x, "cancelled by the receiver");
			case ZRQINIT:
				xfer_fail(z->x, "the other side is sending, not receiving");
			}
		}
		if (++tries > MAX_ERRORS * 3)
			xfer_fail(z->x, "the receiver didn't answer");
	}
}

/* a ZCRCQ's answer: -1 for its ACK, -2 for a ZSKIP (the receiver
   skipped the file), else the position a ZRPOS asks for */
static long long wait_for_ack(struct z *z, long long pos)
{
	struct header h;
	for (int i = 0; i < MAX_ERRORS; i++) {
		if (!header_or_null(z, &h))
			continue;
		if (h.type == ZACK)
			return -1;
		if (h.type == ZRPOS)
			return hpos(&h);
		if (h.type == ZSKIP)
			return -2;
		if (h.type == ZCAN || h.type == ZABORT)
			xfer_cancelled(z->x, "cancelled by the receiver");
	}
	xfer_fail(z->x, "no answer at %lld", pos);
}

/* the open file positioned at to */
static long long seek(struct z *z, long long to)
{
	if (fseek(z->x->src, (long)to, SEEK_SET) != 0)
		xfer_fail(z->x, "can't go to %lld in the file", to);
	return to;
}

/* the file's data from start in ZCRCG subpackets, a ZCRCQ every WINDOW
   bytes to hear how it goes, ZCRCE and ZEOF at the end; a ZRPOS from the
   receiver puts it back there; done at the receiver's ZRINIT */
static void stream(struct z *z, long long start)
{
	dmc_xfer *x = z->x;
	unsigned char buf[BLOCK];
	int errors = 0;
	long long at = start, last_back = -1, pos;
	struct header h;
	/* (errors counted while no progress is made) */
#define BACK(to) do { \
		long long to_ = (to); \
		if (to_ > last_back) \
			errors = 0; \
		last_back = to_; \
		if (++errors > MAX_ERRORS) \
			xfer_fail(x, "too many errors on the line"); \
		xfer_error(x); \
		at = to_; \
		xfer_sleep(20); \
		xfer_purge(x); \
	} while (0)
	for (;;) {
		pos = seek(z, at);
		send_bin(z, hdr_pos(ZDATA, pos));
		int since_ack = 0, restart = 0;
		for (;;) {
			/* the receiver's word in between, if any (what isn't a
			   header's start let go first: an XON, a CR LF) */
			for (;;) {
				int c = xfer_peek(x);
				if (c < 0 || c == ZPAD || c == ZDLE)
					break;
				xfer_read(x, 0);
			}
			if (xfer_available(x) && header_or_null(z, &h)) {
				if (h.type == ZRPOS) {
					BACK(hpos(&h));
					restart = 1;
					break;
				}
				if (h.type == ZCAN || h.type == ZABORT)
					xfer_cancelled(x, "cancelled by the receiver");
				if (h.type == ZSKIP)
					return;
			}
			int len = 0;
			while (len < BLOCK) {
				int r = xfer_src_read(x, buf + len, BLOCK - len);
				if (r <= 0)
					break;
				len += r;
			}
			pos += len;
			since_ack += len;
			int end = len < BLOCK ? ZCRCE : since_ack >= WINDOW ? ZCRCQ : ZCRCG;
			send_data(z, buf, len, end);
			xfer_bytes(x, pos);
			if (end == ZCRCE)
				break;
			if (end == ZCRCQ) {
				since_ack = 0;
				long long to = wait_for_ack(z, pos);
				if (to == -2)
					return;
				if (to >= 0) {
					BACK(to);
					restart = 1;
					break;
				}
			}
		}
		if (restart)
			continue;
		/* ZEOF until the receiver's ZRINIT (it has the file), or a
		   ZRPOS (it wants some again) */
		for (;;) {
			send_bin(z, hdr_pos(ZEOF, pos));
			if (!header_or_null(z, &h)) {
				if (++errors > MAX_ERRORS)
					xfer_fail(x, "the end of the file wasn't answered");
				continue;
			}
			if (h.type == ZRINIT || h.type == ZSKIP)
				return;
			if (h.type == ZRPOS) {
				BACK(hpos(&h));
				restart = 1;
				break;
			}
			if (h.type == ZCAN || h.type == ZABORT)
				xfer_cancelled(x, "cancelled by the receiver");
			if (h.type != ZACK && ++errors > MAX_ERRORS)
				xfer_fail(x, "the end of the file wasn't answered");
		}
	}
#undef BACK
}

/* a file sent, until the receiver's ZRINIT: 1 if it skipped it */
static int send_file(struct z *z, const struct xfer_file *f, int files_left,
		     long long bytes_left)
{
	unsigned char info[XFER_NAME + 128];
	int n = snprintf((char *)info, XFER_NAME, "%s", f->name);
	if (n >= XFER_NAME)
		n = XFER_NAME - 1;
	info[n++] = 0;
	n += snprintf((char *)info + n, 120, "%lld %llo 0 0 %d %lld",
		      f->size > 0 ? f->size : 0LL,
		      (unsigned long long)(f->mtime > 0 ? f->mtime : 0),
		      files_left, bytes_left);
	info[n++] = 0;
	long long start = -1;
	int tries = 0;
	struct header h;
	while (start < 0) {
		send_bin(z, hdr_flags(ZFILE, z->x->opt.resume ? ZCRECOV : ZCBIN));
		send_data(z, info, n, ZCRCW);
		for (;;) {
			int got = header_or_null(z, &h);
			if (got && h.type == ZRPOS) {
				start = hpos(&h);
				break;
			}
			if (got && h.type == ZSKIP)
				return 1;
			if (got && h.type == ZCRC) {
				send_hex(z, hdr_pos(ZCRC, 0));
				continue;
			}
			if (got && (h.type == ZCAN || h.type == ZABORT))
				xfer_cancelled(z->x, "cancelled by the receiver");
			if (got && h.type == ZFERR)
				xfer_fail(z->x, "the receiver couldn't write the file");
			/* (a ZRINIT repeated, from before the ZFILE: not an
			   answer to it) */
			if (got && h.type == ZRINIT)
				continue;
			if (++tries > MAX_ERRORS)
				xfer_fail(z->x, "the receiver didn't answer the file");
			break;
		}
	}
	xfer_open(z->x, f);
	stream(z, start);
	xfer_close(z->x);
	return 0;
}

void z_send(dmc_xfer *x)
{
	struct z *z = z_new(x);
	struct header r = wait_for_zrinit(z);
	z->crc32 = (r.p[3] & CANFC32) != 0;
	long long left = 0;
	for (int i = 0; i < x->nfiles; i++)
		left += x->files[i].size > 0 ? x->files[i].size : 0;
	for (int i = 0; i < x->nfiles; i++) {
		const struct xfer_file *f = &x->files[i];
		xfer_file_start(x, f->name, f->size);
		int skipped = send_file(z, f, x->nfiles - i, left);
		left -= f->size > 0 ? f->size : 0;
		if (!skipped)
			xfer_file_done(x);
	}
	/* the end: ZFIN, its answer, "OO" */
	struct header h;
	for (int i = 0; i < MAX_ERRORS; i++) {
		send_hex(z, hdr_pos(ZFIN, 0));
		if (header_or_null(z, &h) && h.type == ZFIN)
			break;
	}
	xfer_write(x, "OO", 2);
}
