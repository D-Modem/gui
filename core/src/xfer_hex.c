/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * xfer_hex.c - Intel HEX and Motorola S-records: a binary image as lines
 *              of text, each a record of an address, its data and a
 *              checksum, for EPROM programmers, monitor ROMs and boot
 *              loaders, which take them on a plain serial line.  No
 *              protocol but the records' own checksums.
 *
 * Intel HEX: ":" count, address, type, data, checksum (the two's
 * complement of the bytes' sum); types 00 data, 01 the end, 02 an
 * extended segment address (x16), 04 an extended linear address (the
 * upper 16 bits), 03 and 05 a start address (taken, unused).
 *
 * S-records: "S" type, count (address, data and checksum's bytes),
 * address, data, checksum (the one's complement of the bytes' sum); S0 a
 * header, S1/S2/S3 data at 16/24/32-bit addresses, S5/S6 the count of
 * data records, S7/S8/S9 the end (with a start address).
 *
 * Sent: the file as records, a line each (CR LF), or as it is if it is
 * the format's records already.  Received: the records that come, until
 * the end record (or a pause once some have come), as the binary image
 * they hold (gaps 0xFF, as an erased EPROM has them).
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "xfer.h"

#define RECORD_BYTES 16
/* the largest image put together (64 MB) */
#define MAX_IMAGE (64LL << 20)
#define FIRST_MS 60000
#define PAUSE_MS 10000
/* a line longer than any record: the rest of it dropped */
#define MAX_LINE 600

/* ------------------------------------------------------------------ */
/* reading records */

struct chunk {
	long long addr;
	size_t off;
	int len;
};

struct decoder {
	int srec;
	int ended, records;
	long long bytes, upper;
	struct chunk *chunks;
	int nchunks, cap;
	unsigned char *data;
	size_t dn, dcap;
	char why[200];
};

static void dec_free(void *p)
{
	struct decoder *d = (struct decoder *)p;
	if (!d)
		return;
	free(d->chunks);
	free(d->data);
	free(d);
}

static int put(struct decoder *d, long long addr, const unsigned char *b, int n)
{
	if (d->nchunks == d->cap) {
		int cap = d->cap ? d->cap * 2 : 64;
		struct chunk *c = (struct chunk *)realloc(d->chunks, sizeof(*c) * (size_t)cap);
		if (!c)
			return 0;
		d->chunks = c;
		d->cap = cap;
	}
	if (d->dn + (size_t)n > d->dcap) {
		size_t cap = d->dcap ? d->dcap * 2 : 4096;
		while (cap < d->dn + (size_t)n)
			cap *= 2;
		unsigned char *p = (unsigned char *)realloc(d->data, cap);
		if (!p)
			return 0;
		d->data = p;
		d->dcap = cap;
	}
	if (n > 0)
		memcpy(d->data + d->dn, b, (size_t)n);
	d->chunks[d->nchunks].addr = addr;
	d->chunks[d->nchunks].off = d->dn;
	d->chunks[d->nchunks].len = n;
	d->nchunks++;
	d->dn += (size_t)n;
	d->records++;
	d->bytes += n;
	return 1;
}

/* the bytes the hex digits s spell, into b: how many, -1 if not hex */
static int hex_bytes(const char *s, unsigned char *b, int max)
{
	int n = (int)strlen(s);
	if (n % 2)
		return -1;
	for (int i = 0; i < n; i++)
		if (!isxdigit((unsigned char)s[i]))
			return -1;
	if (n / 2 > max)
		return -1;
	for (int i = 0; i < n / 2; i++) {
		char t[3] = { s[2 * i], s[2 * i + 1], 0 };
		b[i] = (unsigned char)strtol(t, NULL, 16);
	}
	return n / 2;
}

/* a line: 1 a record of the format, 0 not one, -1 a bad one (d->why) */
static int dec_line(struct decoder *d, const char *raw)
{
	char l[MAX_LINE + 1];
	while (*raw && isspace((unsigned char)*raw))
		raw++;
	snprintf(l, sizeof(l), "%s", raw);
	int ln = (int)strlen(l);
	while (ln && isspace((unsigned char)l[ln - 1]))
		l[--ln] = '\0';
	if (d->ended || !ln)
		return 0;
	unsigned char b[MAX_LINE / 2 + 2];
	if (!d->srec) {
		if (l[0] != ':')
			return 0;
		int n = hex_bytes(l + 1, b, (int)sizeof(b));
		if (n < 0) {
			snprintf(d->why, sizeof(d->why),
				 "Intel HEX record %d: not hex digits", d->records + 1);
			return -1;
		}
		if (n < 5 || n != b[0] + 5) {
			snprintf(d->why, sizeof(d->why),
				 "Intel HEX: a record's length is wrong: %.80s", l);
			return -1;
		}
		int sum = 0;
		for (int i = 0; i < n; i++)
			sum += b[i];
		if (sum & 0xff) {
			snprintf(d->why, sizeof(d->why),
				 "Intel HEX: a checksum is wrong: %.80s", l);
			return -1;
		}
		int addr = (b[1] << 8) | b[2];
		switch (b[3]) {
		case 0:
			if (!put(d, d->upper + addr, b + 4, b[0])) {
				snprintf(d->why, sizeof(d->why), "out of memory");
				return -1;
			}
			break;
		case 1:
			d->ended = 1;
			break;
		case 2:
			d->upper = (long long)((b[4] << 8) | b[5]) << 4;
			break;
		case 4:
			d->upper = (long long)((b[4] << 8) | b[5]) << 16;
			break;
		case 3:
		case 5:
			break;
		default:
			snprintf(d->why, sizeof(d->why),
				 "Intel HEX: an unknown record type: %.80s", l);
			return -1;
		}
		return 1;
	}
	if (ln < 2 || (l[0] != 'S' && l[0] != 's') || !isdigit((unsigned char)l[1]))
		return 0;
	int type = l[1] - '0';
	int n = hex_bytes(l + 2, b, (int)sizeof(b));
	if (n < 0) {
		snprintf(d->why, sizeof(d->why), "S-record %d: not hex digits",
			 d->records + 1);
		return -1;
	}
	if (n == 0 || n != b[0] + 1) {
		snprintf(d->why, sizeof(d->why),
			 "S-record: a record's length is wrong: %.80s", l);
		return -1;
	}
	int sum = 0;
	for (int i = 0; i < n; i++)
		sum += b[i];
	if ((sum & 0xff) != 0xff) {
		snprintf(d->why, sizeof(d->why), "S-record: a checksum is wrong: %.80s", l);
		return -1;
	}
	int ab;
	switch (type) {
	case 0: case 1: case 5: case 9: ab = 2; break;
	case 2: case 6: case 8: ab = 3; break;
	case 3: case 7: ab = 4; break;
	default:
		snprintf(d->why, sizeof(d->why), "S-record: an unknown type: %.80s", l);
		return -1;
	}
	if (n < 1 + ab + 1) {
		snprintf(d->why, sizeof(d->why), "S-record: too short: %.80s", l);
		return -1;
	}
	long long addr = 0;
	for (int k = 0; k < ab; k++)
		addr = (addr << 8) | b[1 + k];
	if (type >= 1 && type <= 3) {
		if (!put(d, addr, b + 1 + ab, n - 1 - ab - 1)) {
			snprintf(d->why, sizeof(d->why), "out of memory");
			return -1;
		}
	} else if (type >= 7) {
		d->ended = 1;
	}
	return 1;
}

/* whether text is the format's records already (every line one, the end
   record among them) */
static int is_records(const char *text, size_t n, int srec)
{
	struct decoder d;
	memset(&d, 0, sizeof(d));
	d.srec = srec;
	size_t i = 0;
	int ok = 1;
	while (i < n && ok) {
		size_t e = i;
		while (e < n && text[e] != '\n' && text[e] != '\r')
			e++;
		if (e - i <= MAX_LINE) {
			char l[MAX_LINE + 1];
			memcpy(l, text + i, e - i);
			l[e - i] = '\0';
			int empty = 1;
			for (char *p = l; *p; p++)
				if (!isspace((unsigned char)*p))
					empty = 0;
			if (!empty && dec_line(&d, l) != 1 && !d.ended)
				ok = 0;
		} else {
			ok = 0;
		}
		i = e + 1;
	}
	int ended = d.ended;
	free(d.chunks);
	free(d.data);
	return ok && ended;
}

/* ------------------------------------------------------------------ */
/* writing records */

static const char digits[] = "0123456789ABCDEF";

static int hex_out(char *o, const unsigned char *b, int n)
{
	for (int i = 0; i < n; i++) {
		o[2 * i] = digits[b[i] >> 4];
		o[2 * i + 1] = digits[b[i] & 15];
	}
	return 2 * n;
}

/* a line sent (count: only its length counted), CR LF after it */
static long long emit(dmc_xfer *x, const char *line, int n, int count,
		      long long *sent, long long total, const char *name)
{
	(void)total;
	(void)name;
	if (!count) {
		if (xfer_is_cancelled(x))
			xfer_cancelled(x, "cancelled");
		xfer_write(x, line, n);
		xfer_write(x, "\r\n", 2);
		*sent += n + 2;
		xfer_bytes(x, *sent);
	}
	return n + 2;
}

static int intel_record(char *o, int addr, int type, const unsigned char *data, int n)
{
	unsigned char b[4 + 255 + 1];
	b[0] = (unsigned char)n;
	b[1] = (unsigned char)(addr >> 8);
	b[2] = (unsigned char)addr;
	b[3] = (unsigned char)type;
	if (n > 0)
		memcpy(b + 4, data, (size_t)n);
	int sum = 0;
	for (int i = 0; i < 4 + n; i++)
		sum += b[i];
	b[4 + n] = (unsigned char)(-sum);
	o[0] = ':';
	return 1 + hex_out(o + 1, b, 5 + n);
}

static int srec_record(char *o, int type, int ab, long long addr,
		       const unsigned char *data, int n)
{
	unsigned char b[1 + 4 + 255 + 1];
	int k = 0;
	b[k++] = (unsigned char)(ab + n + 1);
	for (int i = 0; i < ab; i++)
		b[k++] = (unsigned char)(addr >> (8 * (ab - 1 - i)));
	if (n > 0)
		memcpy(b + k, data, (size_t)n);
	k += n;
	int sum = 0;
	for (int i = 0; i < k; i++)
		sum += b[i];
	b[k++] = (unsigned char)~sum;
	o[0] = 'S';
	o[1] = (char)('0' + type);
	return 2 + hex_out(o + 2, b, k);
}

/* data as the format's records (S-records: a header of name first),
   sent, or only counted: their length with CR LFs */
static long long encode(dmc_xfer *x, const unsigned char *data, long long dn,
			int srec, const char *name, int count, long long total)
{
	char l[MAX_LINE];
	long long len = 0, sent = 0;
	if (!srec) {
		long long upper = -1;
		long long i = 0;
		while (i < dn) {
			long long addr = i;
			/* (a record doesn't cross a 64 KB boundary) */
			if ((addr >> 16) != upper) {
				upper = addr >> 16;
				unsigned char u[2] = { (unsigned char)(upper >> 8),
						       (unsigned char)upper };
				len += emit(x, l, intel_record(l, 0, 4, u, 2), count,
					    &sent, total, name);
			}
			long long room = 0x10000 - (addr & 0xffff);
			int n = RECORD_BYTES;
			if (dn - i < n)
				n = (int)(dn - i);
			if (room < n)
				n = (int)room;
			len += emit(x, l, intel_record(l, (int)(addr & 0xffff), 0,
						       data + i, n), count, &sent,
				    total, name);
			i += n;
		}
		len += emit(x, ":00000001FF", 11, count, &sent, total, name);
		return len;
	}
	long long last = (dn > 0 ? dn : 1) - 1;
	int type, ab, end;
	if (last <= 0xffff) {
		type = 1; ab = 2; end = 9;
	} else if (last <= 0xffffff) {
		type = 2; ab = 3; end = 8;
	} else {
		type = 3; ab = 4; end = 7;
	}
	int nl = (int)strlen(name);
	if (nl > 60)
		nl = 60;
	len += emit(x, l, srec_record(l, 0, 2, 0, (const unsigned char *)name, nl),
		    count, &sent, total, name);
	long long i = 0, records = 0;
	while (i < dn) {
		int n = dn - i < RECORD_BYTES ? (int)(dn - i) : RECORD_BYTES;
		len += emit(x, l, srec_record(l, type, ab, i, data + i, n), count,
			    &sent, total, name);
		i += n;
		records++;
	}
	if (records <= 0xffff)
		len += emit(x, l, srec_record(l, 5, 2, records, NULL, 0), count,
			    &sent, total, name);
	len += emit(x, l, srec_record(l, end, ab, 0, NULL, 0), count, &sent,
		    total, name);
	return len;
}

/* the file's records, sent as they are: their length, or sent */
static long long send_as_is(dmc_xfer *x, const char *text, size_t n, int count,
			    long long total, const char *name)
{
	long long len = 0, sent = 0;
	size_t i = 0;
	while (i < n) {
		size_t e = i;
		while (e < n && text[e] != '\n' && text[e] != '\r')
			e++;
		size_t a = i, b = e;
		while (a < b && isspace((unsigned char)text[a]))
			a++;
		while (b > a && isspace((unsigned char)text[b - 1]))
			b--;
		if (b > a)
			len += emit(x, text + a, (int)(b - a), count, &sent, total, name);
		i = e + 1;
	}
	return len;
}

void hex_send(dmc_xfer *x, int srec)
{
	const struct xfer_file *f = &x->files[0];
	const char *label = srec ? "S-records" : "Intel HEX";
	xfer_open(x, f);
	size_t cap = 65536, n = 0;
	unsigned char *data = (unsigned char *)malloc(cap + 1);
	if (!data)
		xfer_fail(x, "out of memory");
	x->heap = data;
	for (;;) {
		if (n == cap) {
			if ((long long)cap >= MAX_IMAGE)
				xfer_fail(x, "%s: too big for %s", f->name, label);
			unsigned char *p = (unsigned char *)realloc(data, cap * 2 + 1);
			if (!p)
				xfer_fail(x, "out of memory");
			data = p;
			x->heap = data;
			cap *= 2;
		}
		int r = xfer_src_read(x, data + n, (int)(cap - n > 65536 ? 65536 : cap - n));
		if (r <= 0)
			break;
		n += (size_t)r;
	}
	xfer_close(x);
	data[n] = 0;
	long long total;
	if (is_records((const char *)data, n, srec)) {
		total = send_as_is(x, (const char *)data, n, 1, 0, f->name);
		xfer_file_start(x, f->name, total);
		send_as_is(x, (const char *)data, n, 0, total, f->name);
	} else {
		total = encode(x, data, (long long)n, srec, f->name, 1, 0);
		xfer_file_start(x, f->name, total);
		encode(x, data, (long long)n, srec, f->name, 0, total);
	}
	xfer_bytes(x, total);
	xfer_file_done(x);
}

void hex_receive(dmc_xfer *x, int srec)
{
	const char *label = srec ? "S-record" : "Intel HEX";
	struct decoder *d = (struct decoder *)calloc(1, sizeof(*d));
	if (!d)
		xfer_fail(x, "out of memory");
	d->srec = srec;
	x->proto = d;
	x->proto_free = dec_free;
	char line[MAX_LINE + 1];
	int ln = 0;
	xfer_file_start(x, x->xname, -1);
	while (!d->ended) {
		int c = xfer_read(x, d->records == 0 ? FIRST_MS : PAUSE_MS);
		if (c < 0) {
			if (d->records == 0)
				xfer_fail(x, "no %s records came", label);
			/* (a sender that sends no end record: done once it
			   pauses) */
			break;
		}
		if (c == '\r' || c == '\n') {
			if (ln) {
				line[ln] = '\0';
				if (dec_line(d, line) < 0)
					xfer_fail(x, "%s", d->why);
				ln = 0;
				xfer_bytes(x, d->bytes);
			}
		} else if (ln < MAX_LINE) {
			line[ln++] = (char)c;
		}
	}
	/* the image from the lowest address to the end of the highest
	   record */
	long long first = 0, end = 0;
	for (int i = 0; i < d->nchunks; i++) {
		long long a = d->chunks[i].addr, e = a + d->chunks[i].len;
		if (i == 0 || a < first)
			first = a;
		if (i == 0 || e > end)
			end = e;
	}
	if (end - first > MAX_IMAGE)
		xfer_fail(x, "the records span %lld bytes: too many for an image",
			  end - first);
	size_t size = (size_t)(end - first);
	unsigned char *img = (unsigned char *)malloc(size ? size : 1);
	if (!img)
		xfer_fail(x, "out of memory");
	x->heap = img;
	memset(img, 0xff, size);
	/* (in the order they came: a record written again wins) */
	for (int i = 0; i < d->nchunks; i++)
		memcpy(img + (d->chunks[i].addr - first), d->data + d->chunks[i].off,
		       (size_t)d->chunks[i].len);
	xfer_create(x, x->xname);
	xfer_dst_write(x, img, (int)size);
	xfer_done(x, 0);
	xfer_file_start(x, x->dst_name, (long long)size);
	xfer_bytes(x, (long long)size);
	xfer_file_done(x);
}
