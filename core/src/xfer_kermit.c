/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * xfer_kermit.c - Kermit, both ways, stop-and-wait: packets of printable
 *                 characters, control characters prefixed, the 8th bit
 *                 prefixed when the line is 7-bit, runs of a byte
 *                 counted; long packets, 1-, 2- and 3-character (CRC)
 *                 checks, file attributes (size and time), as both sides
 *                 agree in the S/Y exchange.  A bad or missing packet is
 *                 asked for again (NAK) up to MAX_RETRIES times; an E
 *                 packet ends it.
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "xfer.h"

#define MARK 1
#define CR 13
#define OUR_QCTL '#'
#define REPT '~'
#define CAP_LP 2
#define CAP_AT 8
/* the long packets asked for */
#define OUR_LONG 4000
#define MAX_RETRIES 10
#define TIMEOUT 10000
#define MAX_PACKET 9100

#define tochar(v) ((v) + 32)
#define unchar(v) ((v) - 32)
#define ctl(v) ((v) ^ 64)

struct kermit {
	dmc_xfer *x;
	int seven_bit;
	/* what's agreed: the defaults until the S/Y exchange */
	int send_max, send_long, eol, npad, padc, their_qctl, qbin, rept;
	int check, attributes;
	volatile int seq;               /* the next packet's (a cancel's E too) */
	volatile int force_check1;      /* during the S/Y exchange: type 1 */
	int last_sent_type;
	/* the packet that came */
	int pseq, ptype, plen;
	unsigned char pkt[MAX_PACKET + 16];
	/* an ACK's data, kept */
	unsigned char ack[MAX_PACKET + 16];
	int ack_len;
	unsigned char obuf[MAX_PACKET + 64];
};

/* CRC-16/KERMIT (CCITT, bits reflected, from 0) */
static unsigned kcrc(const unsigned char *b, int n)
{
	unsigned c = 0;
	for (int i = 0; i < n; i++) {
		c ^= b[i];
		for (int k = 0; k < 8; k++)
			c = c & 1 ? (c >> 1) ^ 0x8408 : c >> 1;
	}
	return c & 0xffff;
}

/* the block check of type 1, 2 or 3 over n bytes into out: its length */
static int check_chars(int type, const unsigned char *b, int n, unsigned char *out)
{
	int s = 0;
	for (int i = 0; i < n; i++)
		s += b[i];
	if (type == 1) {
		out[0] = (unsigned char)tochar((s + ((s & 192) >> 6)) & 63);
		return 1;
	}
	if (type == 2) {
		out[0] = (unsigned char)tochar((s >> 6) & 63);
		out[1] = (unsigned char)tochar(s & 63);
		return 2;
	}
	unsigned c = kcrc(b, n);
	out[0] = (unsigned char)tochar((c >> 12) & 0x0f);
	out[1] = (unsigned char)tochar((c >> 6) & 63);
	out[2] = (unsigned char)tochar(c & 63);
	return 3;
}

static int check1(const unsigned char *b, int n)
{
	unsigned char c[3];
	check_chars(1, b, n, c);
	return c[0];
}

/* the check type a packet goes with: 1 for S, I and R (and in the S/Y
   exchange), else the agreed one */
static int check_len(int check, int force1, int type)
{
	return type == 'S' || type == 'I' || type == 'R' || force1 ? 1 : check;
}

/* a packet's bytes into out: padding, MARK, its header (an extended one
   for long data), data, its check and EOL; their length */
static int packet_bytes(unsigned char *out, int npad, int padc, int eol,
			int check, int force1, int seq, int type,
			const unsigned char *data, int n)
{
	int ct = check_len(check, force1, type), k = 0;
	for (int i = 0; i < npad; i++)
		out[k++] = (unsigned char)padc;
	out[k++] = MARK;
	int body = k;
	if (n + 2 + ct <= 94) {
		out[k++] = (unsigned char)tochar(n + 2 + ct);
		out[k++] = (unsigned char)tochar(seq);
		out[k++] = (unsigned char)type;
	} else {
		int lx = n + ct;
		out[k++] = (unsigned char)tochar(0);
		out[k++] = (unsigned char)tochar(seq);
		out[k++] = (unsigned char)type;
		out[k++] = (unsigned char)tochar(lx / 95);
		out[k++] = (unsigned char)tochar(lx % 95);
		out[k] = (unsigned char)check1(out + body, 5);
		k++;
	}
	if (n > 0)
		memcpy(out + k, data, (size_t)n);
	k += n;
	k += check_chars(ct, out + body, k - body, out + k);
	out[k++] = (unsigned char)eol;
	return k;
}

static void send_packet(struct kermit *k, int seq, int type,
			const unsigned char *data, int n)
{
	int len = packet_bytes(k->obuf, k->npad, k->padc, k->eol, k->check,
			       k->force_check1, seq, type, data, n);
	xfer_write(k->x, k->obuf, len);
}

/* the next good packet into k (1), or 0 on a timeout or a bad one
   (garbage before its mark skipped; a mark in the middle starts over) */
static int read_packet(struct kermit *k, int wait)
{
	long long deadline = xfer_now() + wait;
	unsigned char *buf = k->pkt;
#define NEXT() (xfer_now() >= deadline ? -1 : xfer_read(k->x, (int)(deadline - xfer_now())))
	int c = NEXT();
	for (;;) {
		while (c != MARK) {
			if (c < 0)
				return 0;
			c = NEXT();
		}
		int n = 0, v, restart = 0;
		int hdr[3];
		for (int i = 0; i < 3 && !restart; i++) {
			v = NEXT();
			if (v < 0)
				return 0;
			if (v == MARK)
				restart = 1;
			else
				hdr[i] = buf[n++] = (unsigned char)v;
		}
		if (restart) {
			c = MARK;
			continue;
		}
		int len = unchar(hdr[0]), pseq = unchar(hdr[1]), type = hdr[2], rest;
		if (len == 0) {
			/* extended: LENX1 LENX2 HCHECK */
			int xx[3];
			for (int i = 0; i < 3 && !restart; i++) {
				v = NEXT();
				if (v < 0)
					return 0;
				if (v == MARK)
					restart = 1;
				else
					xx[i] = buf[n++] = (unsigned char)v;
			}
			if (restart) {
				c = MARK;
				continue;
			}
			if (check1(buf, 5) != xx[2])
				return 0;
			rest = unchar(xx[0]) * 95 + unchar(xx[1]);
		} else {
			if (len < 3)
				return 0;
			rest = len - 2;
		}
		if (rest > MAX_PACKET || rest < 0)
			return 0;
		int header_len = n;
		for (int i = 0; i < rest && !restart; i++) {
			v = NEXT();
			if (v < 0)
				return 0;
			if (v == MARK)
				restart = 1;
			else
				buf[n++] = (unsigned char)v;
		}
		if (restart) {
			c = MARK;
			continue;
		}
		int ct;
		if (type == 'S' || type == 'I' || type == 'R' || k->force_check1)
			ct = 1;
		else if (type == 'Y' && pseq == 0 && k->last_sent_type == 'S')
			ct = 1;         /* (an S packet's ACK: type 1 too) */
		else
			ct = k->check;
		if (rest < ct)
			return 0;
		int data_end = n - ct;
		unsigned char want[3];
		check_chars(ct, buf, data_end, want);
		if (memcmp(buf + data_end, want, (size_t)ct) != 0) {
			/* (an E or N packet sent before the check type changed) */
			if ((type == 'N' || type == 'E') && ct != 1 && rest >= 1) {
				unsigned char w1[3];
				check_chars(1, buf, n - 1, w1);
				if (buf[n - 1] == w1[0]) {
					k->pseq = pseq;
					k->ptype = type;
					k->plen = n - 1 - header_len;
					memmove(buf, buf + header_len, (size_t)k->plen);
					return 1;
				}
			}
			return 0;
		}
		k->pseq = pseq;
		k->ptype = type;
		k->plen = data_end - header_len;
		memmove(buf, buf + header_len, (size_t)k->plen);
		return 1;
	}
#undef NEXT
}

/* ------------------------------------------------------------------ */
/* the data's encoding */

static void encode_byte(struct kermit *k, unsigned char *out, int *n, int v)
{
	int b = v;
	if (k->qbin && (b & 0x80)) {
		out[(*n)++] = (unsigned char)k->qbin;
		b &= 0x7f;
	}
	int b7 = b & 0x7f;
	if (b7 < 32 || b7 == 127) {
		out[(*n)++] = OUR_QCTL;
		out[(*n)++] = (unsigned char)ctl(b);
	} else if (b7 == OUR_QCTL || (k->qbin && b7 == k->qbin) ||
		   (k->rept && b7 == k->rept)) {
		out[(*n)++] = OUR_QCTL;
		out[(*n)++] = (unsigned char)b;
	} else {
		out[(*n)++] = (unsigned char)b;
	}
}

/* src's len bytes encoded into at most max characters of out (*on of
   them): how many bytes they hold */
static int encode(struct kermit *k, const unsigned char *src, int len,
		  unsigned char *out, int max, int *on)
{
	int i = 0, n = 0;
	while (i < len) {
		int v = src[i], run = 1;
		if (k->rept)
			while (i + run < len && run < 94 && src[i + run] == src[i])
				run++;
		unsigned char unit[8];
		int un = 0;
		if (run >= 3) {
			unit[un++] = (unsigned char)k->rept;
			unit[un++] = (unsigned char)tochar(run);
			encode_byte(k, unit, &un, v);
		} else {
			run = 1;
			encode_byte(k, unit, &un, v);
		}
		if (n + un > max)
			break;
		memcpy(out + n, unit, (size_t)un);
		n += un;
		i += run;
	}
	*on = n;
	return i;
}

/* a packet's data decoded: into the received file, else into text (up
   to tmax bytes); the bytes it held */
static long long decode(struct kermit *k, const unsigned char *d, int dn,
			char *text, int tmax)
{
	unsigned char out[4096];
	int on = 0, tn = 0;
	long long total = 0;
	int i = 0;
	while (i < dn) {
		int run = 1, c = d[i++];
		if (k->rept && c == k->rept && i < dn) {
			run = unchar(d[i++]);
			if (i >= dn)
				break;
			c = d[i++];
		}
		int hi = 0;
		if (k->qbin && c == k->qbin && i < dn) {
			hi = 0x80;
			c = d[i++];
		}
		if (c == k->their_qctl && i < dn) {
			c = d[i++];
			int c7 = c & 0x7f;
			if (c7 >= 63 && c7 <= 95)
				c = ctl(c);
		}
		c |= hi;
		for (int r = 0; r < run; r++) {
			if (text) {
				if (tn < tmax - 1)
					text[tn++] = (char)c;
			} else {
				out[on++] = (unsigned char)c;
				if (on == (int)sizeof(out)) {
					xfer_dst_write(k->x, out, on);
					on = 0;
				}
			}
			total++;
		}
	}
	if (text)
		text[tn] = '\0';
	else if (on)
		xfer_dst_write(k->x, out, on);
	return total;
}

/* an E packet with why sent (if it can be), and the transfer failed */
static void kfail(struct kermit *k, const char *why)
{
	unsigned char e[600];
	int n;
	encode(k, (const unsigned char *)why, (int)strlen(why), e, (int)sizeof(e), &n);
	if (!xfer_is_cancelled(k->x))
		send_packet(k, k->seq, 'E', e, n);
	xfer_fail(k->x, "%s", why);
}

int kermit_cancel_bytes(dmc_xfer *x, char *out, int max)
{
	struct kermit *k = (struct kermit *)x->proto;
	int check = k ? k->check : 1, force1 = k ? k->force_check1 : 1;
	int eol = k ? k->eol : CR, seq = k ? k->seq : 0;
	unsigned char b[64];
	int n = packet_bytes(b, 0, 0, eol, check, force1, seq, 'E',
			     (const unsigned char *)"cancelled", 9);
	if (n > max)
		n = max;
	memcpy(out, b, (size_t)n);
	return n;
}

/* ------------------------------------------------------------------ */
/* the S/Y exchange */

/* our Send-Init: MAXL, TIME, NPAD, PADC, EOL, QCTL, QBIN, CHKT, REPT,
   CAPAS, WINDO, MAXLX1, MAXLX2 */
static int our_params(struct kermit *k, unsigned char *b)
{
	int n = 0, t = TIMEOUT / 1000;
	b[n++] = tochar(94);
	b[n++] = (unsigned char)tochar(t < 1 ? 1 : t > 94 ? 94 : t);
	b[n++] = tochar(0);
	b[n++] = ctl(0);
	b[n++] = tochar(CR);
	b[n++] = OUR_QCTL;
	b[n++] = k->seven_bit ? '&' : 'Y';
	b[n++] = '3';
	b[n++] = REPT;
	b[n++] = tochar(CAP_LP | CAP_AT);
	b[n++] = tochar(1);
	b[n++] = tochar(OUR_LONG / 95);
	b[n++] = tochar(OUR_LONG % 95);
	return n;
}

static int is_prefix(int c)
{
	return (c >= 33 && c <= 62) || (c >= 96 && c <= 126);
}

/* their parameters, agreed with ours */
static void agree(struct kermit *k, const unsigned char *d, int dn)
{
#define AT(i) ((i) < dn ? (int)d[i] : -1)
	if (AT(0) > 32) {
		int m = unchar(AT(0));
		k->send_max = m < 10 ? 10 : m > 94 ? 94 : m;
	}
	k->npad = AT(2) > 32 ? unchar(AT(2)) : 0;
	k->padc = AT(3) >= 0 ? ctl(AT(3)) : 0;
	k->eol = AT(4) > 32 ? unchar(AT(4)) : CR;
	k->their_qctl = AT(5) > 32 ? AT(5) : '#';
	int our_qbin = k->seven_bit ? '&' : 'Y', their_qbin = AT(6);
	if (their_qbin == 'Y' && our_qbin != 'Y')
		k->qbin = our_qbin;
	else if (is_prefix(their_qbin) && (our_qbin == 'Y' || our_qbin == their_qbin))
		k->qbin = their_qbin;
	else
		k->qbin = 0;
	int their_check = AT(7) >= '1' && AT(7) <= '3' ? AT(7) - '0' : 1;
	k->check = their_check;
	k->rept = AT(8) == REPT ? REPT : 0;
	/* the capabilities (more bytes follow while the lowest bit is set) */
	int i = 9, caps = 0;
	if (AT(i) > 32) {
		caps = unchar(AT(i));
		while (AT(i) >= 0 && (unchar(AT(i)) & 1))
			i++;
	}
	i++;
	k->attributes = (caps & CAP_AT) != 0;
	k->send_long = 0;
	if (caps & CAP_LP) {
		/* WINDO, then MAXLX1 MAXLX2 (none: 500) */
		int x1 = AT(i + 1), x2 = AT(i + 2);
		k->send_long = x1 > 32 || x2 > 32 ?
			unchar(x1 > 32 ? x1 : 32) * 95 + unchar(x2 > 32 ? x2 : 32) : 500;
		if (k->send_long < 95)
			k->send_long = 0;
		if (k->send_long > OUR_LONG)
			k->send_long = OUR_LONG;
	}
	if (k->qbin && k->qbin == k->rept)
		k->rept = 0;
#undef AT
}

/* room for data in a packet (an encoded byte is up to 5 characters) */
static int room(struct kermit *k)
{
	return k->send_long > 0 ? k->send_long - 5 - k->check - 2 :
		k->send_max - 2 - k->check;
}

static struct kermit *k_new(dmc_xfer *x)
{
	struct kermit *k = (struct kermit *)calloc(1, sizeof(*k));
	if (!k)
		xfer_fail(x, "out of memory");
	k->x = x;
	k->send_max = 80;
	k->eol = CR;
	k->their_qctl = '#';
	k->check = 1;
	k->force_check1 = 1;
	k->last_sent_type = ' ';
	x->proto = k;
	return k;
}

/* ------------------------------------------------------------------ */
/* sending */

/* a packet sent until its ACK comes (a NAK of the next one counts): the
   ACK's data in k->ack */
static void exchange(struct kermit *k, int type, const unsigned char *data, int n)
{
	int tries = 0;
	k->last_sent_type = type;
	for (;;) {
		send_packet(k, k->seq, type, data, n);
		while (read_packet(k, TIMEOUT)) {
			if (k->ptype == 'E') {
				char t[512];
				decode(k, k->pkt, k->plen, t, (int)sizeof(t));
				xfer_fail(k->x, "the receiver: %s", t);
			}
			if (k->ptype == 'Y' && k->pseq == k->seq) {
				k->seq = (k->seq + 1) & 63;
				memcpy(k->ack, k->pkt, (size_t)k->plen);
				k->ack_len = k->plen;
				return;
			}
			if (k->ptype == 'N' && k->pseq == ((k->seq + 1) & 63)) {
				k->seq = (k->seq + 1) & 63;
				k->ack_len = 0;
				return;
			}
			if (k->ptype == 'N')
				break;
			/* (an old ACK, repeated: the next one waited for) */
		}
		if (++tries > MAX_RETRIES)
			kfail(k, "too many retries");
		xfer_error(k->x);
	}
}

static void attr(unsigned char *a, int *n, int tag, const char *v)
{
	int l = (int)strlen(v);
	a[(*n)++] = (unsigned char)tag;
	a[(*n)++] = (unsigned char)tochar(l);
	memcpy(a + *n, v, (size_t)l);
	*n += l;
}

/* seconds as an attribute's date in local time: yyyymmdd hh:mm:ss */
static void format_date(long long sec, char *out, int n)
{
	time_t t = (time_t)sec;
	struct tm *tm = localtime(&t);
	if (!tm) {
		out[0] = '\0';
		return;
	}
	snprintf(out, n, "%04d%02d%02d %02d:%02d:%02d", tm->tm_year + 1900,
		 tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
}

/* "[yy]yymmdd[ hh:mm[:ss]]", local time: seconds, 0 when it isn't one */
static long long parse_date(const char *v)
{
	while (*v == ' ')
		v++;
	int digits = 0;
	while (isdigit((unsigned char)v[digits]))
		digits++;
	if (digits != 8 && digits != 6)
		return 0;
	struct tm tm;
	memset(&tm, 0, sizeof(tm));
	const char *d = v;
	if (digits == 8) {
		tm.tm_year = (d[0] - '0') * 1000 + (d[1] - '0') * 100 +
			(d[2] - '0') * 10 + (d[3] - '0') - 1900;
		d += 4;
	} else {
		int yy = (d[0] - '0') * 10 + (d[1] - '0');
		tm.tm_year = yy < 70 ? yy + 100 : yy;
		d += 2;
	}
	tm.tm_mon = (d[0] - '0') * 10 + (d[1] - '0') - 1;
	tm.tm_mday = (d[2] - '0') * 10 + (d[3] - '0');
	const char *p = v + digits;
	while (*p == ' ' || *p == '\t')
		p++;
	int h = 0, m = 0, s = 0;
	if (p != v + digits && sscanf(p, "%d:%d:%d", &h, &m, &s) < 2) {
		h = m = s = 0;
	}
	tm.tm_hour = h;
	tm.tm_min = m;
	tm.tm_sec = s;
	tm.tm_isdst = -1;
	time_t t = mktime(&tm);
	return t == (time_t)-1 ? 0 : (long long)t;
}

/* a file sent: F, its attributes if agreed (A), D packets, Z (the file
   skipped if the receiver refuses or stops it) */
static void send_file(struct kermit *k, const struct xfer_file *f)
{
	dmc_xfer *x = k->x;
	/* (the encoded packet's and the file's: in x->heap, freed at the end) */
	unsigned char *enc = (unsigned char *)x->heap, *buf = enc + OUR_LONG + 64;
	int n;
	xfer_file_start(x, f->name, f->size);
	encode(k, (const unsigned char *)f->name, (int)strlen(f->name), enc,
	       OUR_LONG, &n);
	exchange(k, 'F', enc, n);
	if (k->attributes) {
		unsigned char a[200];
		int an = 0;
		char v[64];
		attr(a, &an, '"', "B8");                /* binary, 8-bit */
		if (f->size >= 0) {
			snprintf(v, sizeof(v), "%lld", (f->size + 1023) / 1024);
			attr(a, &an, '!', v);
			snprintf(v, sizeof(v), "%lld", f->size);
			attr(a, &an, '1', v);
		}
		if (f->mtime > 0) {
			format_date(f->mtime, v, sizeof(v));
			if (v[0])
				attr(a, &an, '#', v);
		}
		exchange(k, 'A', a, an);
		if (k->ack_len && k->ack[0] == 'N') {
			/* refused: the file skipped */
			exchange(k, 'Z', (const unsigned char *)"D", 1);
			return;
		}
	}
	xfer_open(x, f);
	int have = 0, eof = 0;
	long long sent = 0;
	for (;;) {
		while (!eof && have < OUR_LONG) {
			int r = xfer_src_read(x, buf + have, OUR_LONG - have);
			if (r <= 0)
				eof = 1;
			else
				have += r;
		}
		if (!have)
			break;
		int used = encode(k, buf, have, enc, room(k), &n);
		exchange(k, 'D', enc, n);
		memmove(buf, buf + used, (size_t)(have - used));
		have -= used;
		sent += used;
		xfer_bytes(x, sent);
		if (k->ack_len && (k->ack[0] == 'X' || k->ack[0] == 'Z')) {
			/* the receiver wants no more of this file (or of any) */
			int all = k->ack[0] == 'Z';
			exchange(k, 'Z', (const unsigned char *)"D", 1);
			xfer_close(x);
			if (all)
				xfer_cancelled(x, "cancelled by the receiver");
			return;
		}
	}
	xfer_close(x);
	exchange(k, 'Z', NULL, 0);
	xfer_file_done(x);
}

void kermit_send(dmc_xfer *x)
{
	struct kermit *k = k_new(x);
	unsigned char p[32];
	x->heap = malloc(OUR_LONG + 64 + OUR_LONG);
	if (!x->heap)
		xfer_fail(x, "out of memory");
	k->seq = 0;
	k->force_check1 = 1;
	exchange(k, 'S', p, our_params(k, p));
	agree(k, k->ack, k->ack_len);
	k->force_check1 = 0;
	for (int i = 0; i < x->nfiles; i++)
		send_file(k, &x->files[i]);
	exchange(k, 'B', NULL, 0);
}

/* ------------------------------------------------------------------ */
/* receiving */

static void ack(struct kermit *k, int seq, const unsigned char *data, int n)
{
	k->last_sent_type = 'Y';
	send_packet(k, seq, 'Y', data, n);
}

static void nak(struct kermit *k, int seq)
{
	k->last_sent_type = 'N';
	send_packet(k, seq, 'N', NULL, 0);
}

void kermit_receive(dmc_xfer *x)
{
	struct kermit *k = k_new(x);
	int tries = 0;
	char text[600];
	k->seq = 0;
	k->force_check1 = 1;
	/* waiting for the S (a NAK now and then, for a sender waiting) */
	for (;;) {
		int got = read_packet(k, TIMEOUT);
		if (got && k->ptype == 'S')
			break;
		if (got && k->ptype == 'E') {
			decode(k, k->pkt, k->plen, text, (int)sizeof(text));
			xfer_fail(x, "the sender: %s", text);
		}
		if (++tries > MAX_RETRIES)
			xfer_fail(x, "the sender didn't start");
		nak(k, 0);
	}
	agree(k, k->pkt, k->plen);
	unsigned char ours[32];
	int on = our_params(k, ours);
	int init_seq = k->pseq;
	ack(k, init_seq, ours, on);
	/* our last ACK, for when it is lost */
	int last_seq = init_seq;
	memcpy(k->ack, ours, (size_t)on);
	k->ack_len = on;
	k->force_check1 = 0;
	k->seq = (init_seq + 1) & 63;
	long long mtime = 0, got_bytes = 0;
	tries = 0;
	unsigned char reply[XFER_NAME * 2 + 16];
	for (;;) {
		int got = read_packet(k, TIMEOUT);
		if (!got || k->pseq != k->seq) {
			if (got && k->pseq == last_seq) {
				/* our ACK lost: sent again */
				if (k->ptype == 'S')
					k->force_check1 = 1;
				ack(k, last_seq, k->ack, k->ack_len);
				k->force_check1 = 0;
			} else if (got && k->ptype == 'E') {
				decode(k, k->pkt, k->plen, text, (int)sizeof(text));
				xfer_fail(x, "the sender: %s", text);
			} else {
				nak(k, k->seq);
			}
			if (++tries > MAX_RETRIES)
				kfail(k, "too many retries");
			xfer_error(x);
			continue;
		}
		tries = 0;
		int rn = 0;
		switch (k->ptype) {
		case 'F': {
			decode(k, k->pkt, k->plen, text, XFER_NAME);
			/* (a name sent "converted", all capitals, as C-Kermit
			   does: back in lower case) */
			int letters = 0, lower = 0;
			for (char *p = text; *p; p++) {
				if (isalpha((unsigned char)*p))
					letters = 1;
				if (islower((unsigned char)*p))
					lower = 1;
			}
			if (letters && !lower)
				for (char *p = text; *p; p++)
					*p = (char)tolower((unsigned char)*p);
			xfer_create(x, text);
			mtime = 0;
			got_bytes = 0;
			xfer_file_start(x, x->dst_name, -1);
			encode(k, (const unsigned char *)x->dst_name,
			       (int)strlen(x->dst_name), reply, (int)sizeof(reply), &rn);
			break;
		}
		case 'A': {
			const unsigned char *a = k->pkt;
			int i = 0;
			while (i + 1 < k->plen) {
				int tag = a[i], n = unchar(a[i + 1]);
				if (n < 0 || i + 2 + n > k->plen)
					break;
				char v[128];
				snprintf(v, sizeof(v), "%.*s", n, (const char *)a + i + 2);
				if (tag == '1') {
					xfer_size(x, strtoll(v, NULL, 10));
				} else if (tag == '!') {
					if (xfer_size_now(x) < 0)
						xfer_size(x, strtoll(v, NULL, 10) * 1024);
				} else if (tag == '#') {
					mtime = parse_date(v);
				}
				i += 2 + n;
			}
			reply[rn++] = 'Y';
			break;
		}
		case 'D':
			if (!x->dst)
				kfail(k, "data before a file's name");
			got_bytes += decode(k, k->pkt, k->plen, NULL, 0);
			xfer_bytes(x, got_bytes);
			break;
		case 'Z':
			if (x->dst) {
				if (k->plen && k->pkt[0] == 'D') {
					xfer_abandon(x);
				} else {
					xfer_done(x, mtime);
					xfer_file_done(x);
				}
			}
			break;
		case 'B':
			ack(k, k->pseq, NULL, 0);
			return;
		case 'E':
			decode(k, k->pkt, k->plen, text, (int)sizeof(text));
			xfer_fail(x, "the sender: %s", text);
		case 'X':
			/* text to show (a server's): taken like a file with no
			   name */
			break;
		default: {
			char why[64];
			snprintf(why, sizeof(why), "unexpected %c packet", k->ptype);
			kfail(k, why);
		}
		}
		int this_seq = k->pseq;
		ack(k, this_seq, reply, rn);
		last_seq = this_seq;
		memcpy(k->ack, reply, (size_t)rn);
		k->ack_len = rn;
		k->seq = (k->seq + 1) & 63;
	}
}
