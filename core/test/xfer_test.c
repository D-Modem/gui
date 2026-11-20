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
static void link2(dmc_xfer *a, dmc_xfer *b, int noise)
{
	char buf[4096];
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
	printf("plain text\n");
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
