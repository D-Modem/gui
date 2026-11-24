/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * xfer_test.c - the file transfers both ways, one transfer's line the
 *               other's: each protocol, each file as it was sent (and
 *               its time), on a clean line and on one that damages
 *               bytes; a cancel; what starts a transfer.  With lrzsz's
 *               sz and rz and C-Kermit (when they are installed, or
 *               DMODEM_XFER_INTEROP=1 makes them needed), the same
 *               against them on a pseudo-terminal.
 */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include "dmodemctl.h"
#include "xfer.h"
#include "zstd_codec.h"

static int failures;

#define CHECK(c, ...) do { \
		if (!(c)) { \
			failures++; \
			fprintf(stderr, "%s:%d: ", __FILE__, __LINE__); \
			fprintf(stderr, __VA_ARGS__); \
			fputc('\n', stderr); \
		} \
	} while (0)

#define MTIME 1700000000LL

static char tmp[512];

static void write_file(const char *path, const unsigned char *d, size_t n)
{
	FILE *f = fopen(path, "wb");
	fwrite(d, 1, n, f);
	fclose(f);
	struct utimbuf u = { (time_t)MTIME, (time_t)MTIME };
	utime(path, &u);
}

static unsigned char *read_file(const char *path, size_t *n)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		*n = 0;
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long l = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *d = (unsigned char *)malloc((size_t)l + 1);
	*n = fread(d, 1, (size_t)l, f);
	fclose(f);
	return d;
}

static long long mtime_of(const char *path)
{
	struct stat sb;
	return stat(path, &sb) == 0 ? (long long)sb.st_mtime : -1;
}

static void empty_dir(const char *dir)
{
	char cmd[1024];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", dir, dir);
	if (system(cmd) != 0)
		abort();
}

/* the two transfers' line, until both end: every noise-th byte from a
   damaged (0 none) */
static long long line_bytes;             /* what a sent in the last link2 */

static void link2(dmc_xfer *a, dmc_xfer *b, int noise)
{
	char buf[4096];
	line_bytes = 0;
	long long count = 0, end = xfer_now() + 180000;
	for (;;) {
		dmc_xfer_status sa, sb;
		dmc_xfer_get_status(a, &sa);
		dmc_xfer_get_status(b, &sb);
		if (!sa.running && !sb.running)
			break;
		if (xfer_now() > end) {
			CHECK(0, "the transfer took too long");
			dmc_xfer_cancel(a);
			dmc_xfer_cancel(b);
		}
		int moved = 0, n;
		while ((n = dmc_xfer_output(a, buf, sizeof(buf))) > 0) {
			for (int i = 0; noise && i < n; i++)
				if (++count % noise == 0)
					buf[i] ^= 0x55;
			dmc_xfer_feed(b, buf, n);
			line_bytes += n;
			moved = 1;
		}
		while ((n = dmc_xfer_output(b, buf, sizeof(buf))) > 0) {
			dmc_xfer_feed(a, buf, n);
			moved = 1;
		}
		if (!moved)
			xfer_sleep(1);
	}
}

static unsigned char big[40000], small[] = "hello, modem\n";

static void check_same(const char *path, const unsigned char *d, size_t n,
		       int prefix, const char *what)
{
	size_t got;
	unsigned char *g = read_file(path, &got);
	CHECK(g != NULL, "%s: %s not there", what, path);
	if (!g)
		return;
	if (prefix)
		CHECK(got >= n && !memcmp(g, d, n), "%s: %s differs (%zu, %zu)",
		      what, path, got, n);
	else
		CHECK(got == n && !memcmp(g, d, n), "%s: %s differs (%zu, %zu)",
		      what, path, got, n);
	free(g);
}

/* the protocol both ways, the files as they were */
static void both_ways(int p, int noise)
{
	char src[600], dst[600], a[700], b[700];
	snprintf(src, sizeof(src), "%s/src", tmp);
	snprintf(dst, sizeof(dst), "%s/dst", tmp);
	empty_dir(src);
	empty_dir(dst);
	snprintf(a, sizeof(a), "%s/data.bin", src);
	snprintf(b, sizeof(b), "%s/hello.txt", src);
	write_file(a, big, sizeof(big));
	write_file(b, small, sizeof(small) - 1);
	const char *paths[2] = { a, b };
	dmc_xfer *r = dmc_xfer_receive(p, dst, "data.bin");
	dmc_xfer *s = dmc_xfer_send(p, paths, 2);
	link2(s, r, noise);
	dmc_xfer_status ss, rs;
	dmc_xfer_get_status(s, &ss);
	dmc_xfer_get_status(r, &rs);
	const char *what = dmc_xfer_label(p);
	CHECK(!ss.result[0], "%s (noise %d): the sender: %s", what, noise, ss.result);
	CHECK(!rs.result[0], "%s (noise %d): the receiver: %s", what, noise, rs.result);
	int batch = dmc_xfer_batch(p);
	int xmodem = p == DMC_XFER_XMODEM || p == DMC_XFER_XMODEM_1K;
	char got[700];
	snprintf(got, sizeof(got), "%s/data.bin", dst);
	check_same(got, big, sizeof(big), xmodem, what);
	if (batch) {
		snprintf(got, sizeof(got), "%s/hello.txt", dst);
		check_same(got, small, sizeof(small) - 1, 0, what);
		CHECK(rs.files == 2, "%s: %d files", what, rs.files);
		CHECK(dmc_xfer_received(r, 1) && !strcmp(dmc_xfer_received(r, 1), "hello.txt"),
		      "%s: the second name", what);
		CHECK(mtime_of(got) == MTIME, "%s: the time %lld", what, mtime_of(got));
	}
	if (noise)
		CHECK(rs.errors + ss.errors > 0, "%s: no errors on a noisy line", what);
	dmc_xfer_free(s);
	dmc_xfer_free(r);
}

/* YMODEM-G: an error ends it on both sides (no block again); a sender
   asked with C does plain YMODEM */
static void ymodem_g(void)
{
	char src[600], dst[600], a[700], got[700];
	snprintf(src, sizeof(src), "%s/src", tmp);
	snprintf(dst, sizeof(dst), "%s/dst", tmp);
	empty_dir(src);
	empty_dir(dst);
	snprintf(a, sizeof(a), "%s/data.bin", src);
	write_file(a, big, sizeof(big));
	const char *paths[1] = { a };
	dmc_xfer *r = dmc_xfer_receive(DMC_XFER_YMODEM_G, dst, NULL);
	dmc_xfer *s = dmc_xfer_send(DMC_XFER_YMODEM_G, paths, 1);
	link2(s, r, 3001);
	dmc_xfer_status ss, rs;
	dmc_xfer_get_status(s, &ss);
	dmc_xfer_get_status(r, &rs);
	CHECK(strstr(rs.result, "YMODEM-G") != NULL, "YMODEM-G noisy: the receiver: '%s'",
	      rs.result);
	CHECK(ss.result[0], "YMODEM-G noisy: the sender went on");
	snprintf(got, sizeof(got), "%s/data.bin", dst);
	FILE *f = fopen(got, "rb");
	CHECK(!f, "YMODEM-G noisy: the part of the file was kept");
	if (f)
		fclose(f);
	dmc_xfer_free(s);
	dmc_xfer_free(r);

	empty_dir(dst);
	r = dmc_xfer_receive(DMC_XFER_YMODEM, dst, NULL);
	s = dmc_xfer_send(DMC_XFER_YMODEM_G, paths, 1);
	link2(s, r, 0);
	dmc_xfer_get_status(s, &ss);
	dmc_xfer_get_status(r, &rs);
	CHECK(!ss.result[0] && !rs.result[0], "YMODEM-G to YMODEM: '%s' '%s'",
	      ss.result, rs.result);
	check_same(got, big, sizeof(big), 0, "YMODEM-G to YMODEM");
	dmc_xfer_free(s);
	dmc_xfer_free(r);
}

/* link2 with the receiver's skip once it has some bytes of the first file */
static void link2_skip(dmc_xfer *a, dmc_xfer *b)
{
	char buf[4096];
	int skipped = 0;
	long long end = xfer_now() + 120000;
	for (;;) {
		dmc_xfer_status sa, sb;
		dmc_xfer_get_status(a, &sa);
		dmc_xfer_get_status(b, &sb);
		if (!sa.running && !sb.running)
			break;
		if (xfer_now() > end) {
			CHECK(0, "the skip: it took too long");
			dmc_xfer_cancel(a);
			dmc_xfer_cancel(b);
		}
		if (!skipped && sb.bytes > 8000 && !strcmp(sb.file, "data.bin")) {
			dmc_xfer_skip(b);
			skipped = 1;
		}
		int moved = 0, n;
		/* (a slow line: 4 KB a turn) */
		if ((n = dmc_xfer_output(a, buf, sizeof(buf))) > 0) {
			dmc_xfer_feed(b, buf, n);
			moved = 1;
		}
		while ((n = dmc_xfer_output(b, buf, sizeof(buf))) > 0) {
			dmc_xfer_feed(a, buf, n);
			moved = 1;
		}
		xfer_sleep(moved ? 1 : 2);
	}
}

/* Skip File: the first file dropped, the second received */
static void skip_file(int p)
{
	char src[600], dst[600], a[700], b[700], got[700];
	const char *what = dmc_xfer_label(p);
	snprintf(src, sizeof(src), "%s/src", tmp);
	snprintf(dst, sizeof(dst), "%s/dst", tmp);
	empty_dir(src);
	empty_dir(dst);
	snprintf(a, sizeof(a), "%s/data.bin", src);
	snprintf(b, sizeof(b), "%s/hello.txt", src);
	write_file(a, big, sizeof(big));
	write_file(b, small, sizeof(small) - 1);
	const char *paths[2] = { a, b };
	dmc_xfer *r = dmc_xfer_receive(p, dst, NULL);
	dmc_xfer *s = dmc_xfer_send(p, paths, 2);
	link2_skip(s, r);
	dmc_xfer_status ss, rs;
	dmc_xfer_get_status(s, &ss);
	dmc_xfer_get_status(r, &rs);
	CHECK(!ss.result[0] && !rs.result[0], "%s skip: '%s' '%s'", what, ss.result,
	      rs.result);
	snprintf(got, sizeof(got), "%s/data.bin", dst);
	FILE *f = fopen(got, "rb");
	CHECK(!f, "%s skip: the skipped file is there", what);
	if (f)
		fclose(f);
	snprintf(got, sizeof(got), "%s/hello.txt", dst);
	check_same(got, small, sizeof(small) - 1, 0, what);
	CHECK(rs.files == 1, "%s skip: %d files", what, rs.files);
	dmc_xfer_free(s);
	dmc_xfer_free(r);
}

/* ZMODEM crash recovery: a part kept when the transfer stops, and
   continued the next time */
static void resume(void)
{
	char src[600], dst[600], a[700], got[700];
	snprintf(src, sizeof(src), "%s/src", tmp);
	snprintf(dst, sizeof(dst), "%s/dst", tmp);
	empty_dir(src);
	empty_dir(dst);
	snprintf(a, sizeof(a), "%s/data.bin", src);
	write_file(a, big, sizeof(big));
	snprintf(got, sizeof(got), "%s/data.bin", dst);
	const char *paths[1] = { a };
	dmc_xfer_options o;
	memset(&o, 0, sizeof(o));
	o.resume = 1;

	/* a part there: the receiver's option continues it */
	write_file(got, big, 15000);
	dmc_xfer *r = dmc_xfer_receive_opt(DMC_XFER_ZMODEM, dst, NULL, &o);
	dmc_xfer *s = dmc_xfer_send(DMC_XFER_ZMODEM, paths, 1);
	long long t0 = xfer_now();
	link2(s, r, 0);
	(void)t0;
	dmc_xfer_status ss, rs;
	dmc_xfer_get_status(s, &ss);
	dmc_xfer_get_status(r, &rs);
	CHECK(!ss.result[0] && !rs.result[0], "resume: '%s' '%s'", ss.result, rs.result);
	check_same(got, big, sizeof(big), 0, "resume (the receiver's option)");
	dmc_xfer_free(s);
	dmc_xfer_free(r);

	/* the same with the sender's ZCRECOV */
	write_file(got, big, 20000);
	r = dmc_xfer_receive(DMC_XFER_ZMODEM, dst, NULL);
	s = dmc_xfer_send_opt(DMC_XFER_ZMODEM, paths, 1, &o);
	link2(s, r, 0);
	dmc_xfer_get_status(r, &rs);
	CHECK(!rs.result[0], "resume (ZCRECOV): '%s'", rs.result);
	check_same(got, big, sizeof(big), 0, "resume (the sender's ZCRECOV)");
	dmc_xfer_free(s);
	dmc_xfer_free(r);

	/* there whole: skipped, and not made again as "data (2).bin" */
	r = dmc_xfer_receive_opt(DMC_XFER_ZMODEM, dst, NULL, &o);
	s = dmc_xfer_send(DMC_XFER_ZMODEM, paths, 1);
	link2(s, r, 0);
	dmc_xfer_get_status(r, &rs);
	CHECK(!rs.result[0], "resume (whole): '%s'", rs.result);
	char two[700];
	snprintf(two, sizeof(two), "%s/data (2).bin", dst);
	FILE *f = fopen(two, "rb");
	CHECK(!f, "resume (whole): a second copy was made");
	if (f)
		fclose(f);
	dmc_xfer_free(s);
	dmc_xfer_free(r);

	/* stopped in the middle: the part kept */
	remove(got);
	r = dmc_xfer_receive_opt(DMC_XFER_ZMODEM, dst, NULL, &o);
	s = dmc_xfer_send(DMC_XFER_ZMODEM, paths, 1);
	char buf[4096];
	for (int turns = 0; turns < 20000; turns++) {
		dmc_xfer_status st;
		dmc_xfer_get_status(r, &st);
		if (st.bytes > 10000)
			break;
		int n;
		if ((n = dmc_xfer_output(s, buf, 1024)) > 0)
			dmc_xfer_feed(r, buf, n);
		while ((n = dmc_xfer_output(r, buf, sizeof(buf))) > 0)
			dmc_xfer_feed(s, buf, n);
		xfer_sleep(1);
	}
	dmc_xfer_cancel(r);
	dmc_xfer_cancel(s);
	link2(s, r, 0);
	size_t n = 0;
	unsigned char *part = read_file(got, &n);
	CHECK(part && n > 0 && n < sizeof(big) && !memcmp(part, big, n),
	      "resume: the part was not kept (%zu bytes)", n);
	free(part);
	dmc_xfer_free(s);
	dmc_xfer_free(r);
}

/* ------------------------------------------------------------------ */
/* ZMODEM with zstd */

static int have(const char *prog);

/* n bytes of text that compresses, as a BBS's text files do */
static unsigned char *text(size_t n, unsigned seed)
{
	static const char *const words[] = {
		"modem", "carrier", "the", "BBS", "file", "area", "sysop",
		"message", "ZMODEM", "of", "and", "a", "upload", "download",
		"baud", "line", "\r\n"
	};
	unsigned char *d = (unsigned char *)malloc(n + 16);
	size_t at = 0;
	unsigned r = seed * 2654435761u + 1;
	while (at < n) {
		r = r * 1103515245u + 12345u;
		const char *w = words[(r >> 16) % (sizeof(words) / sizeof(words[0]))];
		size_t l = strlen(w);
		for (size_t i = 0; i < l && at < n; i++)
			d[at++] = (unsigned char)w[i];
		if (at < n)
			d[at++] = ' ';
	}
	return d;
}

/* the codec: the specification's frame, there and back, against the
   reference zstd (where it is installed) */
static void zstd_codec(void)
{
	static const unsigned char vec[55] = {
		0x28, 0xb5, 0x2f, 0xfd, 0x24, 0x66, 0x55, 0x01, 0x00, 0x24, 0x02, 0x44, 0x2d, 0x4d,
		0x6f, 0x64, 0x65, 0x6d, 0x20, 0x5a, 0x4d, 0x4f, 0x44, 0x45, 0x4d, 0x20, 0x7a, 0x73,
		0x74, 0x64, 0x20, 0x74, 0x65, 0x73, 0x74, 0x20, 0x76, 0x65, 0x63, 0x74, 0x6f, 0x72,
		0x2e, 0x0d, 0x0a, 0x01, 0x00, 0x8a, 0x82, 0xaa, 0x27, 0x32, 0x21, 0x69, 0x9d
	};
	const char *line = "D-Modem ZMODEM zstd test vector.\r\n";
	char want[103];
	snprintf(want, sizeof(want), "%s%s%s", line, line, line);
	int n;
	const char *why;
	unsigned char *d = zc_decode(vec, 55, 1024, &n, &why);
	CHECK(d && n == 102 && !memcmp(d, want, 102), "zstd: the vector: %s", why ? why : "differs");
	free(d);
	CHECK(zc_frame_len(vec, 55, 1024) == 55 && zc_frame_len(vec, 54, 1024) == 0,
	      "zstd: the vector's length");
	unsigned char bad[55];
	memcpy(bad, vec, 55);
	bad[20] ^= 1;
	d = zc_decode(bad, 55, 1024, &n, &why);
	CHECK(!d, "zstd: a byte wrong, taken");
	free(d);
	d = zc_decode(vec, 55, 100, &n, &why);
	CHECK(!d, "zstd: over the limit, taken");
	free(d);
	CHECK(zc_xxh64(NULL, 0) == 0xEF46DB3751D8E999ULL &&
	      zc_xxh64((const unsigned char *)"a", 1) == 0xD24EC4F1A98C6E5BULL &&
	      zc_xxh64((const unsigned char *)"Nobody inspects the spammish repetition", 39) ==
	      0xFBCEA83C8A378BF1ULL, "zstd: XXH64");

	int ref = have("zstd");
	size_t sizes[] = { 0, 1, 255, 256, 65535, 65791, 65792, 200000 };
	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		for (int kind = 0; kind < 3; kind++) {
			size_t sz = sizes[i];
			unsigned char *t = kind == 0 ? text(sz, (unsigned)i) : (unsigned char *)malloc(sz + 1);
			for (size_t k = 0; kind > 0 && k < sz; k++)
				t[k] = kind == 1 ? big[k % sizeof(big)] : (unsigned char)(k / 1000);
			int fn = 0, back = 0;
			unsigned char *f = zc_encode(t, (int)sz, &fn);
			d = f ? zc_decode(f, fn, 256 * 1024, &back, &why) : NULL;
			CHECK(d && (size_t)back == sz && !memcmp(d, t, sz),
			      "zstd: %zu bytes (%d) there and back: %s", sz, kind, why ? why : "differs");
			CHECK(f && zc_frame_len(f, fn, 256 * 1024) == fn, "zstd: %zu bytes: its length", sz);
			if (kind == 0 && sz == 65535)
				CHECK(fn * 3 < (int)sz, "zstd: 64 KB of text in %d bytes", fn);
			/* the reference zstd: ours decoded, and its frame decoded */
			if (sz > 0 && ref && f) {
				char a[700], b[700], cmd[1500];
				snprintf(a, sizeof(a), "%s/z.raw", tmp);
				snprintf(b, sizeof(b), "%s/z.zst", tmp);
				write_file(b, f, (size_t)fn);
				snprintf(cmd, sizeof(cmd), "zstd -q -d -f -o '%s' '%s'", a, b);
				CHECK(system(cmd) == 0, "zstd -d: our frame of %zu bytes", sz);
				check_same(a, t, sz, 0, "zstd -d");
				write_file(a, t, sz);
				snprintf(cmd, sizeof(cmd), "zstd -q -19 --check -f -o '%s' '%s'", b, a);
				CHECK(system(cmd) == 0, "zstd -19: %zu bytes", sz);
				size_t rn;
				unsigned char *r = read_file(b, &rn);
				unsigned char *rd = r ? zc_decode(r, (int)rn, 1 << 23, &back, &why) : NULL;
				CHECK(rd && (size_t)back == sz && !memcmp(rd, t, sz),
				      "zstd: the reference's frame of %zu bytes: %s", sz, why ? why : "differs");
				free(r);
				free(rd);
			}
			free(d);
			free(f);
			free(t);
		}
	}
	if (ref)
		printf("zstd: against the reference zstd\n");
}

/* one ZMODEM transfer of the files (paths) to dst with the two sides'
   options: both ends' results checked */
static void zstd_link(const char *const *paths, int n, const char *dst,
		      const dmc_xfer_options *so, const dmc_xfer_options *ro,
		      int noise, const char *what)
{
	dmc_xfer *r = dmc_xfer_receive_opt(DMC_XFER_ZMODEM, dst, NULL, ro);
	dmc_xfer *s = dmc_xfer_send_opt(DMC_XFER_ZMODEM, paths, n, so);
	link2(s, r, noise);
	dmc_xfer_status ss, rs;
	dmc_xfer_get_status(s, &ss);
	dmc_xfer_get_status(r, &rs);
	CHECK(!ss.result[0], "%s: the sender: %s", what, ss.result);
	CHECK(!rs.result[0], "%s: the receiver: %s", what, rs.result);
	if (noise)
		CHECK(rs.errors + ss.errors > 0, "%s: no errors on a noisy line", what);
	dmc_xfer_free(s);
	dmc_xfer_free(r);
}

/* ZMODEM with zstd: smaller on the line, whole after errors, resumed,
   plain when one side doesn't take it or the file is compressed */
static void zstd_transfers(void)
{
	char src[600], dst[600], p[5][700], got[700];
	const char *paths[5];
	size_t sizes[5] = { 0, 1, 65536, 65537, 300001 };
	unsigned char *d[5];
	long long total = 0;
	dmc_xfer_options on, off;
	memset(&on, 0, sizeof(on));
	memset(&off, 0, sizeof(off));
	off.no_zstd = 1;
	snprintf(src, sizeof(src), "%s/src", tmp);
	snprintf(dst, sizeof(dst), "%s/dst", tmp);
	empty_dir(src);
	empty_dir(dst);
	for (int i = 0; i < 5; i++) {
		d[i] = text(sizes[i], (unsigned)i + 1);
		snprintf(p[i], sizeof(p[i]), "%s/f%d.txt", src, i);
		write_file(p[i], d[i], sizes[i]);
		paths[i] = p[i];
		total += (long long)sizes[i];
	}
	zstd_link(paths, 5, dst, &on, &on, 0, "zstd");
	for (int i = 0; i < 5; i++) {
		snprintf(got, sizeof(got), "%s/f%d.txt", dst, i);
		check_same(got, d[i], sizes[i], 0, "zstd");
	}
	CHECK(line_bytes * 2 < total, "zstd: %lld bytes on the line for %lld", line_bytes, total);
	printf("ZMODEM with zstd: %lld bytes on the line for %lld\n", line_bytes, total);

	/* on a noisy line */
	empty_dir(dst);
	zstd_link(paths + 4, 1, dst, &on, &on, 3001, "zstd (noisy)");
	snprintf(got, sizeof(got), "%s/f4.txt", dst);
	check_same(got, d[4], sizes[4], 0, "zstd (noisy)");

	/* zstd stopped on either side, or a compressed file: plain */
	for (int k = 0; k < 3; k++) {
		char zip[700];
		const char *one[1] = { paths[4] };
		empty_dir(dst);
		if (k == 2) {
			snprintf(zip, sizeof(zip), "%s/f4.zip", src);
			write_file(zip, d[4], sizes[4]);
			one[0] = zip;
		}
		zstd_link(one, 1, dst, k == 0 ? &off : &on, k == 1 ? &off : &on, 0, "zstd off");
		snprintf(got, sizeof(got), "%s/f4.%s", dst, k == 2 ? "zip" : "txt");
		check_same(got, d[4], sizes[4], 0, "zstd off");
		CHECK(line_bytes >= (long long)sizes[4],
		      "zstd off (%d): %lld bytes on the line: compressed", k, line_bytes);
	}

	/* crash recovery with zstd: from the end of the part (fewer bytes on
	   the line than for the whole file) */
	dmc_xfer_options res = on;
	res.resume = 1;
	empty_dir(dst);
	zstd_link(paths + 4, 1, dst, &on, &on, 0, "zstd (whole)");
	long long whole = line_bytes;
	empty_dir(dst);
	snprintf(got, sizeof(got), "%s/f4.txt", dst);
	write_file(got, d[4], 123457);
	zstd_link(paths + 4, 1, dst, &on, &res, 0, "zstd (resume)");
	check_same(got, d[4], sizes[4], 0, "zstd (resume)");
	CHECK(line_bytes * 4 < whole * 3,
	      "zstd (resume): %lld bytes on the line, %lld for all: not resumed", line_bytes, whole);
	for (int i = 0; i < 5; i++)
		free(d[i]);
}

/* plain text with the waits of ASCII Setup */
static void text_delays(void)
{
	char src[600], dst[600], a[700], got[700];
	snprintf(src, sizeof(src), "%s/src", tmp);
	snprintf(dst, sizeof(dst), "%s/dst", tmp);
	empty_dir(src);
	empty_dir(dst);
	static const unsigned char text[] = "one\r\ntwo\r\nthree\rfour\n";
	snprintf(a, sizeof(a), "%s/t.txt", src);
	write_file(a, text, sizeof(text) - 1);
	const char *paths[1] = { a };
	dmc_xfer_options o;
	memset(&o, 0, sizeof(o));
	o.line_delay_ms = 100;
	o.char_delay_ms = 5;
	xfer_text_quiet_ms = 600;
	dmc_xfer *r = dmc_xfer_receive(DMC_XFER_TEXT, dst, "t.txt");
	long long t0 = xfer_now();
	dmc_xfer *s = dmc_xfer_send_opt(DMC_XFER_TEXT, paths, 1, &o);
	for (;;) {
		dmc_xfer_status st;
		dmc_xfer_get_status(s, &st);
		char buf[256];
		int n;
		while ((n = dmc_xfer_output(s, buf, sizeof(buf))) > 0)
			dmc_xfer_feed(r, buf, n);
		if (!st.running)
			break;
		xfer_sleep(1);
	}
	long long took = xfer_now() - t0;
	/* 4 line ends and 21 characters */
	CHECK(took >= 4 * 100 + 21 * 5, "text delays: %lld ms", took);
	link2(s, r, 0);
	snprintf(got, sizeof(got), "%s/t.txt", dst);
	check_same(got, text, sizeof(text) - 1, 0, "text delays");
	dmc_xfer_free(s);
	dmc_xfer_free(r);
}

static void hex_both_ways(int p)
{
	char src[600], dst[600], a[700], got[700];
	snprintf(src, sizeof(src), "%s/src", tmp);
	snprintf(dst, sizeof(dst), "%s/dst", tmp);
	/* (70000 bytes: past 64 KB, an extended address, S2 records) */
	static unsigned char img[70000];
	for (size_t i = 0; i < sizeof(img); i++)
		img[i] = (unsigned char)(i * 7 + 3);
	for (int as_records = 0; as_records < 2; as_records++) {
		empty_dir(src);
		empty_dir(dst);
		snprintf(a, sizeof(a), "%s/image.bin", src);
		write_file(a, img, sizeof(img));
		if (as_records) {
			/* the records already: sent as they are */
			char d2[600], rec[700];
			snprintf(d2, sizeof(d2), "%s/rec", tmp);
			empty_dir(d2);
			const char *pa[1] = { a };
			dmc_xfer *r = dmc_xfer_receive(DMC_XFER_TEXT, d2, "image.hex");
			dmc_xfer *s = dmc_xfer_send(p, pa, 1);
			xfer_text_quiet_ms = 300;
			link2(s, r, 0);
			dmc_xfer_free(s);
			dmc_xfer_free(r);
			snprintf(rec, sizeof(rec), "%s/image.hex", d2);
			snprintf(a, sizeof(a), "%s", rec);
		}
		const char *paths[1] = { a };
		dmc_xfer *r = dmc_xfer_receive(p, dst, "image.bin");
		dmc_xfer *s = dmc_xfer_send(p, paths, 1);
		link2(s, r, 0);
		dmc_xfer_status rs;
		dmc_xfer_get_status(r, &rs);
		CHECK(!rs.result[0], "%s: %s", dmc_xfer_label(p), rs.result);
		snprintf(got, sizeof(got), "%s/image.bin", dst);
		check_same(got, img, sizeof(img), 0, dmc_xfer_label(p));
		dmc_xfer_free(s);
		dmc_xfer_free(r);
	}
}

static void text_both_ways(void)
{
	char src[600], dst[600], a[700], got[700];
	snprintf(src, sizeof(src), "%s/src", tmp);
	snprintf(dst, sizeof(dst), "%s/dst", tmp);
	empty_dir(src);
	empty_dir(dst);
	snprintf(a, sizeof(a), "%s/note.txt", src);
	write_file(a, big, 5000);
	xfer_text_quiet_ms = 300;
	const char *paths[1] = { a };
	dmc_xfer *r = dmc_xfer_receive(DMC_XFER_TEXT, dst, "note.txt");
	dmc_xfer *s = dmc_xfer_send(DMC_XFER_TEXT, paths, 1);
	link2(s, r, 0);
	snprintf(got, sizeof(got), "%s/note.txt", dst);
	check_same(got, big, 5000, 0, "plain text");
	dmc_xfer_free(s);
	dmc_xfer_free(r);

	/* a cancel keeps what came */
	empty_dir(dst);
	xfer_text_quiet_ms = 60000;
	r = dmc_xfer_receive(DMC_XFER_TEXT, dst, "kept.txt");
	dmc_xfer_feed(r, "some text", 9);
	xfer_sleep(200);
	dmc_xfer_cancel(r);
	dmc_xfer_status rs;
	do {
		xfer_sleep(10);
		dmc_xfer_get_status(r, &rs);
	} while (rs.running);
	CHECK(!rs.result[0], "plain text's cancel: %s", rs.result);
	snprintf(got, sizeof(got), "%s/kept.txt", dst);
	check_same(got, (const unsigned char *)"some text", 9, 0, "plain text's cancel");
	dmc_xfer_free(r);
}

static void cancel(void)
{
	char dst[600];
	snprintf(dst, sizeof(dst), "%s/dst", tmp);
	empty_dir(dst);
	dmc_xfer *r = dmc_xfer_receive(DMC_XFER_ZMODEM, dst, NULL);
	xfer_sleep(100);
	dmc_xfer_cancel(r);
	char buf[256];
	int n, cans = 0;
	dmc_xfer_status rs;
	do {
		xfer_sleep(10);
		dmc_xfer_get_status(r, &rs);
		while ((n = dmc_xfer_output(r, buf, sizeof(buf))) > 0)
			for (int i = 0; i < n; i++)
				cans += buf[i] == 0x18;
	} while (rs.running);
	CHECK(!strcmp(rs.result, "cancelled"), "cancel: %s", rs.result);
	CHECK(cans >= 5, "cancel: %d CANs sent", cans);
	dmc_xfer_free(r);
}

static void detect(void)
{
	int at = -1;
	const char sz[] = "rz waiting\r\n**\x18" "B00000000000000\r\x8a\x11";
	CHECK(dmc_xfer_detect(sz, (int)sizeof(sz) - 1, &at) ==
	      DMC_XFER_DETECT_ZMODEM_SEND && at == 12, "sz %d", at);
	const char rz[] = "**\x18" "B0100000023be50\r\x8a\x11";
	CHECK(dmc_xfer_detect(rz, (int)sizeof(rz) - 1, &at) ==
	      DMC_XFER_DETECT_ZMODEM_RECEIVE && at == 0, "rz");
	const char k[] = "x\x01" "1 S~/ @-#Y3~^>J)0___F\r";
	CHECK(dmc_xfer_detect(k, (int)sizeof(k) - 1, &at) ==
	      DMC_XFER_DETECT_KERMIT_SEND && at == 1, "kermit");
	CHECK(dmc_xfer_detect("hello", 5, &at) == DMC_XFER_DETECT_NONE, "none");
}

/* ------------------------------------------------------------------ */
/* lrzsz and C-Kermit, on a pseudo-terminal */

static int have(const char *prog)
{
	char cmd[256];
	snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", prog);
	return system(cmd) == 0;
}

/* cmd run in dir on a pseudo-terminal (raw), the transfer on its other
   side: the command's exit status */
static int against(dmc_xfer *x, const char *dir, const char *cmd)
{
	int m = posix_openpt(O_RDWR | O_NOCTTY);
	if (m < 0 || grantpt(m) < 0 || unlockpt(m) < 0)
		return -1;
	const char *sname = ptsname(m);
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		int s = open(sname, O_RDWR);
		char raw[700];
		snprintf(raw, sizeof(raw), "stty raw -echo -ixon -ixoff <&%d; cd '%s' && exec %s",
			 s, dir, cmd);
		dup2(s, 0);
		dup2(s, 1);
		int null = open("/dev/null", O_WRONLY);
		dup2(null, 2);
		execl("/bin/sh", "sh", "-c", raw, (char *)NULL);
		_exit(127);
	}
	fcntl(m, F_SETFL, O_NONBLOCK);
	char buf[4096];
	long long end = xfer_now() + 120000;
	int status = -1, done = 0;
	for (;;) {
		int moved = 0;
		ssize_t n = read(m, buf, sizeof(buf));
		if (n > 0) {
			dmc_xfer_feed(x, buf, (int)n);
			moved = 1;
		}
		int k;
		while ((k = dmc_xfer_output(x, buf, sizeof(buf))) > 0) {
			int off = 0;
			while (off < k) {
				ssize_t w = write(m, buf + off, (size_t)(k - off));
				if (w > 0)
					off += (int)w;
				else if (errno == EAGAIN)
					xfer_sleep(1);
				else
					break;
			}
			moved = 1;
		}
		if (!done && waitpid(pid, &status, WNOHANG) == pid)
			done = 1;
		dmc_xfer_status st;
		dmc_xfer_get_status(x, &st);
		if (!st.running && done)
			break;
		if (xfer_now() > end) {
			kill(pid, 9);
			dmc_xfer_cancel(x);
			if (!done)
				waitpid(pid, &status, 0);
			done = 1;
		}
		if (!moved)
			xfer_sleep(2);
	}
	close(m);
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void interop_one(int p, const char *send_cmd, const char *recv_cmd, int prefix)
{
	char src[600], dst[600], a[700], b[700], got[700], cmd[1400];
	const char *what = dmc_xfer_label(p);
	snprintf(src, sizeof(src), "%s/src", tmp);
	snprintf(dst, sizeof(dst), "%s/dst", tmp);
	int batch = dmc_xfer_batch(p);

	/* theirs sends, ours receives */
	empty_dir(src);
	empty_dir(dst);
	snprintf(a, sizeof(a), "%s/data.bin", src);
	snprintf(b, sizeof(b), "%s/hello.txt", src);
	write_file(a, big, sizeof(big));
	write_file(b, small, sizeof(small) - 1);
	snprintf(cmd, sizeof(cmd), "%s data.bin%s", send_cmd, batch ? " hello.txt" : "");
	dmc_xfer *r = dmc_xfer_receive(p, dst, "data.bin");
	int rc = against(r, src, cmd);
	dmc_xfer_status st;
	dmc_xfer_get_status(r, &st);
	CHECK(!st.result[0], "%s from %s: %s", what, send_cmd, st.result);
	CHECK(rc == 0, "%s: %s exited %d", what, send_cmd, rc);
	snprintf(got, sizeof(got), "%s/data.bin", dst);
	check_same(got, big, sizeof(big), prefix, what);
	if (batch) {
		snprintf(got, sizeof(got), "%s/hello.txt", dst);
		check_same(got, small, sizeof(small) - 1, 0, what);
		CHECK(mtime_of(got) == MTIME, "%s from %s: the time %lld", what,
		      send_cmd, mtime_of(got));
	}
	dmc_xfer_free(r);

	/* ours sends, theirs receives */
	empty_dir(dst);
	const char *paths[2] = { a, b };
	dmc_xfer *s = dmc_xfer_send(p, paths, 2);
	rc = against(s, dst, recv_cmd);
	dmc_xfer_get_status(s, &st);
	CHECK(!st.result[0], "%s to %s: %s", what, recv_cmd, st.result);
	CHECK(rc == 0, "%s: %s exited %d", what, recv_cmd, rc);
	snprintf(got, sizeof(got), "%s/data.bin", dst);
	check_same(got, big, sizeof(big), prefix, what);
	if (batch) {
		snprintf(got, sizeof(got), "%s/hello.txt", dst);
		check_same(got, small, sizeof(small) - 1, 0, what);
		CHECK(mtime_of(got) == MTIME, "%s to %s: the time %lld", what,
		      recv_cmd, mtime_of(got));
	}
	dmc_xfer_free(s);
}

static void interop(void)
{
	const char *want = getenv("DMODEM_XFER_INTEROP");
	int need = want && !strcmp(want, "1");
	if (have("sz") && have("rz")) {
		interop_one(DMC_XFER_ZMODEM, "sz -b", "rz -b", 0);
		interop_one(DMC_XFER_YMODEM, "sz --ymodem -b", "rz --ymodem -b", 0);
		/* (sb streams when asked with G; rb asks with C: plain YMODEM) */
		interop_one(DMC_XFER_YMODEM_G, "sz --ymodem -b", "rz --ymodem -b", 0);
		interop_one(DMC_XFER_XMODEM, "sz --xmodem -b", "rz --xmodem -b data.bin", 1);
		interop_one(DMC_XFER_XMODEM_1K, "sz --xmodem -k -b", "rz --xmodem -b data.bin", 1);
		printf("interop: lrzsz\n");
	} else {
		CHECK(!need, "sz and rz are not installed");
	}
	if (have("kermit")) {
		interop_one(DMC_XFER_KERMIT, "kermit -Y -i -s", "kermit -Y -i -r", 0);
		printf("interop: C-Kermit\n");
	} else {
		CHECK(!need, "kermit is not installed");
	}
}

int main(void)
{
	snprintf(tmp, sizeof(tmp), "%s/dmodem-xfer-XXXXXX",
		 getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
	if (!mkdtemp(tmp)) {
		perror("mkdtemp");
		return 1;
	}
	srand(7);
	for (size_t i = 0; i < sizeof(big); i++)
		big[i] = (unsigned char)rand();
	int protocols[] = { DMC_XFER_ZMODEM, DMC_XFER_YMODEM, DMC_XFER_XMODEM,
			    DMC_XFER_XMODEM_1K, DMC_XFER_KERMIT };
	both_ways(DMC_XFER_YMODEM_G, 0);
	ymodem_g();
	printf("YMODEM-G\n");
	for (size_t i = 0; i < sizeof(protocols) / sizeof(protocols[0]); i++) {
		both_ways(protocols[i], 0);
		printf("%s: clean line\n", dmc_xfer_label(protocols[i]));
		/* (Kermit's long packets: most would be damaged at 1 in 3001) */
		both_ways(protocols[i], protocols[i] == DMC_XFER_KERMIT ? 7001 : 3001);
		printf("%s: noisy line\n", dmc_xfer_label(protocols[i]));
	}
	hex_both_ways(DMC_XFER_INTEL_HEX);
	hex_both_ways(DMC_XFER_SREC);
	printf("hex records\n");
	text_both_ways();
	text_delays();
	printf("plain text\n");
	skip_file(DMC_XFER_ZMODEM);
	skip_file(DMC_XFER_KERMIT);
	printf("skip file\n");
	resume();
	printf("ZMODEM crash recovery\n");
	zstd_codec();
	printf("zstd\n");
	zstd_transfers();
	cancel();
	detect();
	interop();
	char cmd[600];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp);
	if (system(cmd) != 0)
		failures++;
	if (failures)
		fprintf(stderr, "%d failures\n", failures);
	else
		printf("all passed\n");
	return failures ? 1 : 0;
}
