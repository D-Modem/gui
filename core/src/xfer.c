/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * xfer.c - a file transfer: its thread, its line (the queues to and from
 *          the remote), its status, the files it reads and writes, and
 *          what starts which protocol.  The protocols are xfer_xy.c,
 *          xfer_z.c, xfer_kermit.c and xfer_hex.c (and plain text, here).
 *          They are the Android app's (org.dmodem.app.transfer), in C.
 */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "xfer.h"

#ifndef _WIN32
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>
#endif

#ifdef _WIN32
#define SEP '\\'
#else
#define SEP '/'
#endif

/* ------------------------------------------------------------------ */
/* the time, the lock */

long long xfer_now(void)
{
#ifdef _WIN32
	/* (GetTickCount wraps after 49 days: kept going past it) */
	static DWORD last;
	static long long high;
	DWORD t = GetTickCount();
	if (t < last)
		high += 0x100000000LL;
	last = t;
	return high + t;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

void xfer_sleep(int ms)
{
#ifdef _WIN32
	Sleep(ms);
#else
	struct timespec ts;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
		;
#endif
}

static void lock(dmc_xfer *x)
{
#ifdef _WIN32
	EnterCriticalSection(&x->cs);
#else
	pthread_mutex_lock(&x->mu);
#endif
}

static void unlock(dmc_xfer *x)
{
#ifdef _WIN32
	LeaveCriticalSection(&x->cs);
#else
	pthread_mutex_unlock(&x->mu);
#endif
}

/* (locked) up to ms for the other thread's word: what came (in), or room
   to send (out) */
static void wait_for(dmc_xfer *x, int in, int ms)
{
#ifdef _WIN32
	unlock(x);
	WaitForSingleObject(in ? x->ev_in : x->ev_out, (DWORD)ms);
	lock(x);
#else
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += ms / 1000;
	ts.tv_nsec += (long)(ms % 1000) * 1000000L;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}
	pthread_cond_timedwait(in ? &x->cv_in : &x->cv_out, &x->mu, &ts);
#endif
}

/* (locked) the app told, once until it takes the output */
static void tell(dmc_xfer *x)
{
	if (x->notify && !x->notified) {
		x->notified = 1;
		x->notify(x->notify_ctx);
	}
}

static void wake(dmc_xfer *x, int in)
{
#ifdef _WIN32
	SetEvent(in ? x->ev_in : x->ev_out);
#else
	pthread_cond_signal(in ? &x->cv_in : &x->cv_out);
#endif
}

/* ------------------------------------------------------------------ */
/* failing */

void xfer_fail(dmc_xfer *x, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(x->why, sizeof(x->why), fmt, ap);
	va_end(ap);
	longjmp(x->jb, 1);
}

void xfer_cancelled(dmc_xfer *x, const char *why)
{
	snprintf(x->why, sizeof(x->why), "%s", why);
	longjmp(x->jb, 2);
}

int xfer_is_cancelled(dmc_xfer *x)
{
	return x->cancelled;
}

/* ------------------------------------------------------------------ */
/* the line, from the protocol's thread */

/* (locked) the next bytes taken into rbuf; 0 if none wait */
static int refill(dmc_xfer *x)
{
	if (!x->in.n)
		return 0;
	x->rlen = buf_take(&x->in, (char *)x->rbuf, (int)sizeof(x->rbuf));
	x->rpos = 0;
	return x->rlen;
}

int xfer_read_nc(dmc_xfer *x, int ms)
{
	if (x->cancelled)
		return -2;
	if (x->rpos < x->rlen)
		return x->rbuf[x->rpos++];
	long long deadline = xfer_now() + ms;
	lock(x);
	for (;;) {
		if (x->cancelled) {
			unlock(x);
			return -2;
		}
		if (refill(x))
			break;
		long long left = deadline - xfer_now();
		if (left <= 0) {
			unlock(x);
			return -1;
		}
		/* (the cancel looked at every quarter second) */
		wait_for(x, 1, left < 250 ? (int)left : 250);
	}
	unlock(x);
	return x->rbuf[x->rpos++];
}

int xfer_read(dmc_xfer *x, int ms)
{
	int c = xfer_read_nc(x, ms);
	if (c == -2)
		xfer_cancelled(x, "cancelled");
	return c;
}

int xfer_peek(dmc_xfer *x)
{
	if (x->rpos < x->rlen)
		return x->rbuf[x->rpos];
	lock(x);
	int got = refill(x);
	unlock(x);
	return got ? x->rbuf[x->rpos] : -1;
}

int xfer_available(dmc_xfer *x)
{
	return xfer_peek(x) >= 0;
}

void xfer_purge(dmc_xfer *x)
{
	x->rpos = x->rlen = 0;
	lock(x);
	buf_clear(&x->in);
	unlock(x);
}

void xfer_write(dmc_xfer *x, const void *data, int n)
{
	if (x->cancelled)
		xfer_cancelled(x, "cancelled");
	if (n <= 0)
		return;
	lock(x);
	while (x->out.n > XFER_OUT_MAX && !x->cancelled)
		wait_for(x, 0, 250);
	if (!x->cancelled) {
		buf_add(&x->out, (const char *)data, n);
		tell(x);
	}
	unlock(x);
	if (x->cancelled)
		xfer_cancelled(x, "cancelled");
}

void xfer_put(dmc_xfer *x, int c)
{
	unsigned char b = (unsigned char)c;
	xfer_write(x, &b, 1);
}

/* ------------------------------------------------------------------ */
/* the status */

void xfer_file_start(dmc_xfer *x, const char *name, long long size)
{
	lock(x);
	snprintf(x->st.file, sizeof(x->st.file), "%s", name);
	x->st.bytes = 0;
	x->st.size = size;
	unlock(x);
}

void xfer_bytes(dmc_xfer *x, long long bytes)
{
	lock(x);
	x->st.bytes = bytes;
	unlock(x);
}

void xfer_size(dmc_xfer *x, long long size)
{
	lock(x);
	x->st.size = size;
	unlock(x);
}

long long xfer_size_now(dmc_xfer *x)
{
	lock(x);
	long long s = x->st.size;
	unlock(x);
	return s;
}

void xfer_file_done(dmc_xfer *x)
{
	lock(x);
	x->st.files++;
	unlock(x);
}

void xfer_error(dmc_xfer *x)
{
	lock(x);
	x->st.errors++;
	unlock(x);
}

/* ------------------------------------------------------------------ */
/* the files */

/* a name with no path in it, nothing a file system minds */
static void safe_name(const char *name, char *out, int n)
{
	const char *base = name;
	for (const char *p = name; *p; p++)
		if (*p == '/' || *p == '\\')
			base = p + 1;
	while (*base == '.' || *base == ' ')
		base++;
	int k = 0;
	for (const char *p = base; *p && k < n - 1 && k < 200; p++) {
		unsigned char c = (unsigned char)*p;
		out[k++] = c < 32 || strchr(":*?\"<>|", c) ? '_' : (char)c;
	}
	while (k > 0 && out[k - 1] == ' ')
		k--;
	out[k] = '\0';
	if (!k)
		snprintf(out, n, "received");
}

static int exists(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (f)
		fclose(f);
	return f != NULL;
}

static void join(char *out, int n, const char *dir, const char *name)
{
	int dl = (int)strlen(dir);
	if (dl && (dir[dl - 1] == '/' || dir[dl - 1] == '\\'))
		snprintf(out, n, "%s%s", dir, name);
	else if (dl)
		snprintf(out, n, "%s%c%s", dir, SEP, name);
	else
		snprintf(out, n, "%s", name);
}

void xfer_create(dmc_xfer *x, const char *name)
{
	if (x->dst)
		xfer_abandon(x);
	char safe[XFER_NAME], try_name[XFER_NAME + 16];
	safe_name(name, safe, sizeof(safe));
	snprintf(try_name, sizeof(try_name), "%s", safe);
	join(x->dst_path, sizeof(x->dst_path), x->folder, try_name);
	/* "name (2).ext" when it's taken */
	const char *dot = strrchr(safe, '.');
	int stem = dot && dot > safe ? (int)(dot - safe) : (int)strlen(safe);
	for (int i = 2; exists(x->dst_path) && i < 10000; i++) {
		snprintf(try_name, sizeof(try_name), "%.*s (%d)%s", stem, safe, i,
			 safe + stem);
		join(x->dst_path, sizeof(x->dst_path), x->folder, try_name);
	}
	x->dst = fopen(x->dst_path, "wb");
	if (!x->dst)
		xfer_fail(x, "can't write %s", try_name);
	snprintf(x->dst_name, sizeof(x->dst_name), "%s", try_name);
}

void xfer_dst_write(dmc_xfer *x, const void *data, int n)
{
	if (n > 0 && fwrite(data, 1, (size_t)n, x->dst) != (size_t)n)
		xfer_fail(x, "can't write %s", x->dst_name);
}

#ifdef _WIN32
/* seconds since 1970 as a FILETIME, and back */
static FILETIME to_filetime(long long t)
{
	unsigned long long v = (unsigned long long)(t + 11644473600LL) * 10000000ULL;
	FILETIME ft;
	ft.dwLowDateTime = (DWORD)v;
	ft.dwHighDateTime = (DWORD)(v >> 32);
	return ft;
}

static long long from_filetime(FILETIME ft)
{
	unsigned long long v = ((unsigned long long)ft.dwHighDateTime << 32) |
		ft.dwLowDateTime;
	return (long long)(v / 10000000ULL) - 11644473600LL;
}
#endif

static void set_mtime(const char *path, long long mtime)
{
#ifdef _WIN32
	HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
			       FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return;
	FILETIME ft = to_filetime(mtime);
	SetFileTime(h, NULL, &ft, &ft);
	CloseHandle(h);
#else
	struct utimbuf u;
	u.actime = u.modtime = (time_t)mtime;
	utime(path, &u);
#endif
}

void xfer_done(dmc_xfer *x, long long mtime)
{
	if (!x->dst)
		return;
	int bad = fclose(x->dst) != 0;
	x->dst = NULL;
	if (bad) {
		remove(x->dst_path);
		xfer_fail(x, "can't write %s", x->dst_name);
	}
	if (mtime > 0)
		set_mtime(x->dst_path, mtime);
	char **n = (char **)realloc(x->names, sizeof(char *) * (size_t)(x->nnames + 1));
	if (n) {
		lock(x);
		x->names = n;
		x->names[x->nnames++] = dup_str(x->dst_name);
		unlock(x);
	}
}

void xfer_abandon(dmc_xfer *x)
{
	if (!x->dst)
		return;
	fclose(x->dst);
	x->dst = NULL;
	remove(x->dst_path);
}

void xfer_open(dmc_xfer *x, const struct xfer_file *f)
{
	xfer_close(x);
	x->src = fopen(f->path, "rb");
	if (!x->src)
		xfer_fail(x, "can't read %s", f->name);
}

void xfer_close(dmc_xfer *x)
{
	if (x->src)
		fclose(x->src);
	x->src = NULL;
}

int xfer_src_read(dmc_xfer *x, void *buf, int n)
{
	int got = (int)fread(buf, 1, (size_t)n, x->src);
	if (got < n && ferror(x->src))
		xfer_fail(x, "can't read the file");
	return got;
}

/* a file to send: its name, size and time */
static int describe(struct xfer_file *f, const char *path)
{
	f->path = dup_str(path);
	const char *base = path;
	for (const char *p = path; *p; p++)
		if (*p == '/' || *p == '\\')
			base = p + 1;
	snprintf(f->name, sizeof(f->name), "%s", base);
	f->size = -1;
	f->mtime = 0;
#ifdef _WIN32
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(path, &fd);
	if (h == INVALID_HANDLE_VALUE)
		return 0;
	FindClose(h);
	f->size = ((long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
	f->mtime = from_filetime(fd.ftLastWriteTime);
#else
	struct stat sb;
	if (stat(path, &sb) < 0)
		return 0;
	f->size = (long long)sb.st_size;
	f->mtime = (long long)sb.st_mtime;
#endif
	return 1;
}

/* ------------------------------------------------------------------ */
/* plain text: a file's bytes as they are, as if typed; received, what
   comes until the line is quiet */

#define TEXT_FIRST_MS 60000

int xfer_text_quiet_ms = 10000;

void text_send(dmc_xfer *x)
{
	const struct xfer_file *f = &x->files[0];
	unsigned char buf[1024];
	long long sent = 0;
	xfer_file_start(x, f->name, f->size);
	xfer_open(x, f);
	for (;;) {
		if (x->cancelled)
			xfer_cancelled(x, "cancelled");
		int n = xfer_src_read(x, buf, (int)sizeof(buf));
		if (n <= 0)
			break;
		xfer_write(x, buf, n);
		sent += n;
		xfer_bytes(x, sent);
	}
	xfer_close(x);
	xfer_file_done(x);
}

void text_receive(dmc_xfer *x)
{
	int c = xfer_read(x, TEXT_FIRST_MS);
	if (c < 0)
		xfer_fail(x, "nothing came");
	xfer_create(x, x->xname);
	xfer_file_start(x, x->dst_name, -1);
	/* (a cancel keeps what came: it is the capture's end) */
	x->keep_on_cancel = 1;
	long long bytes = 0;
	unsigned char buf[1024];
	while (c >= 0) {
		int n = 0;
		buf[n++] = (unsigned char)c;
		while (n < (int)sizeof(buf) && xfer_available(x)) {
			int b = xfer_read_nc(x, 0);
			if (b < 0)
				break;
			buf[n++] = (unsigned char)b;
		}
		xfer_dst_write(x, buf, n);
		bytes += n;
		xfer_bytes(x, bytes);
		c = xfer_read_nc(x, xfer_text_quiet_ms);
	}
	x->keep_on_cancel = 0;
	xfer_done(x, 0);
	xfer_size(x, bytes);
	xfer_file_done(x);
}

/* ------------------------------------------------------------------ */
/* the protocols, by number */

static const char *const labels[DMC_XFER_PROTOCOLS] = {
	"ZMODEM", "YMODEM", "XMODEM", "XMODEM-1K", "Kermit", "Intel HEX",
	"Motorola S-records", "Plain text",
};

const char *dmc_xfer_label(int p)
{
	return p >= 0 && p < DMC_XFER_PROTOCOLS ? labels[p] : "";
}

int dmc_xfer_batch(int p)
{
	return p == DMC_XFER_ZMODEM || p == DMC_XFER_YMODEM || p == DMC_XFER_KERMIT;
}

const char *dmc_xfer_command(int p, int remote_sends)
{
	switch (p) {
	case DMC_XFER_ZMODEM: return remote_sends ? "sz -b " : "rz -b";
	case DMC_XFER_YMODEM: return remote_sends ? "sb " : "rb";
	case DMC_XFER_XMODEM: return remote_sends ? "sx " : "rx -c ";
	case DMC_XFER_XMODEM_1K: return remote_sends ? "sx -k " : "rx -c ";
	case DMC_XFER_KERMIT: return remote_sends ? "kermit -i -s " : "kermit -i -r";
	/* (sent: to whatever takes text; the records: nothing to type) */
	case DMC_XFER_TEXT: return remote_sends ? "cat " : "";
	}
	return "";
}

/* the transfer, on its thread */
static void run(dmc_xfer *x)
{
	int how = setjmp(x->jb);
	if (how == 0) {
		int p = x->protocol;
		if (x->sending) {
			switch (p) {
			case DMC_XFER_ZMODEM: z_send(x); break;
			case DMC_XFER_YMODEM: xy_send_y(x); break;
			case DMC_XFER_XMODEM: xy_send_x(x, 0); break;
			case DMC_XFER_XMODEM_1K: xy_send_x(x, 1); break;
			case DMC_XFER_KERMIT: kermit_send(x); break;
			case DMC_XFER_INTEL_HEX: hex_send(x, 0); break;
			case DMC_XFER_SREC: hex_send(x, 1); break;
			default: text_send(x); break;
			}
		} else {
			switch (p) {
			case DMC_XFER_ZMODEM: z_receive(x); break;
			case DMC_XFER_YMODEM: xy_receive_y(x); break;
			case DMC_XFER_XMODEM:
			case DMC_XFER_XMODEM_1K: xy_receive_x(x); break;
			case DMC_XFER_KERMIT: kermit_receive(x); break;
			case DMC_XFER_INTEL_HEX: hex_receive(x, 0); break;
			case DMC_XFER_SREC: hex_receive(x, 1); break;
			default: text_receive(x); break;
			}
		}
		x->why[0] = '\0';
	} else if (how == 2 && x->keep_on_cancel && x->dst) {
		/* plain text's end: what came kept */
		long long bytes = ftell(x->dst);
		x->keep_on_cancel = 0;
		if (setjmp(x->jb) == 0) {
			xfer_done(x, 0);
			xfer_size(x, bytes);
			xfer_file_done(x);
			x->why[0] = '\0';
		}
	}
	xfer_close(x);
	xfer_abandon(x);
	/* (what the other side still sends of the protocol settles first) */
	if (x->why[0])
		xfer_sleep(300);
	lock(x);
	snprintf(x->st.result, sizeof(x->st.result), "%s", x->why);
	x->st.running = 0;
	tell(x);
	unlock(x);
	/* (after: a cancel reads Kermit's state while it runs) */
	free(x->heap);
	x->heap = NULL;
	if (x->proto_free)
		x->proto_free(x->proto);
	else
		free(x->proto);
	x->proto = NULL;
}

#ifdef _WIN32
static DWORD WINAPI thread_main(LPVOID p)
{
	run((dmc_xfer *)p);
	return 0;
}
#else
static void *thread_main(void *p)
{
	run((dmc_xfer *)p);
	return NULL;
}
#endif

static dmc_xfer *make(int protocol, int sending)
{
	if (protocol < 0 || protocol >= DMC_XFER_PROTOCOLS)
		return NULL;
	dmc_xfer *x = (dmc_xfer *)calloc(1, sizeof(*x));
	if (!x)
		return NULL;
	x->protocol = protocol;
	x->sending = sending;
	x->st.protocol = protocol;
	x->st.sending = sending;
	x->st.running = 1;
	x->st.size = -1;
	/* (the checks' tables made here, on the app's thread) */
	xfer_crc16(0, NULL, 0);
	xfer_crc32(0, NULL, 0);
#ifdef _WIN32
	InitializeCriticalSection(&x->cs);
	x->ev_in = CreateEventA(NULL, FALSE, FALSE, NULL);
	x->ev_out = CreateEventA(NULL, FALSE, FALSE, NULL);
#else
	pthread_mutex_init(&x->mu, NULL);
	pthread_cond_init(&x->cv_in, NULL);
	pthread_cond_init(&x->cv_out, NULL);
#endif
	return x;
}

static dmc_xfer *start(dmc_xfer *x)
{
#ifdef _WIN32
	DWORD id;
	x->thread = CreateThread(NULL, 0, thread_main, x, 0, &id);
	x->started = x->thread != NULL;
#else
	x->started = pthread_create(&x->thread, NULL, thread_main, x) == 0;
#endif
	if (!x->started) {
		snprintf(x->st.result, sizeof(x->st.result), "no thread for it");
		x->st.running = 0;
	}
	return x;
}

dmc_xfer *dmc_xfer_receive(int protocol, const char *folder, const char *name)
{
	dmc_xfer *x = make(protocol, 0);
	if (!x)
		return NULL;
	snprintf(x->folder, sizeof(x->folder), "%s", folder ? folder : "");
	snprintf(x->xname, sizeof(x->xname), "%s",
		 name && *name ? name : "received.bin");
	return start(x);
}

dmc_xfer *dmc_xfer_send(int protocol, const char *const *paths, int n)
{
	if (n <= 0 || !paths)
		return NULL;
	dmc_xfer *x = make(protocol, 1);
	if (!x)
		return NULL;
	if (!dmc_xfer_batch(protocol))
		n = 1;
	x->files = (struct xfer_file *)calloc((size_t)n, sizeof(*x->files));
	if (!x->files) {
		dmc_xfer_free(x);
		return NULL;
	}
	x->nfiles = n;
	for (int i = 0; i < n; i++) {
		if (!describe(&x->files[i], paths[i])) {
			snprintf(x->st.result, sizeof(x->st.result), "can't read %.200s",
				 x->files[i].name);
			x->st.running = 0;
			return x;
		}
	}
	return start(x);
}

void dmc_xfer_feed(dmc_xfer *x, const char *data, int n)
{
	if (!x || n <= 0)
		return;
	lock(x);
	buf_add(&x->in, data, n);
	wake(x, 1);
	unlock(x);
}

int dmc_xfer_output(dmc_xfer *x, char *buf, int max)
{
	if (!x)
		return 0;
	lock(x);
	int n = buf_take(&x->out, buf, max);
	if (n)
		wake(x, 0);
	if (!x->out.n)
		x->notified = 0;
	unlock(x);
	return n;
}

void dmc_xfer_set_notify(dmc_xfer *x, void (*fn)(void *ctx), void *ctx)
{
	if (!x)
		return;
	lock(x);
	x->notify = fn;
	x->notify_ctx = ctx;
	x->notified = 0;
	if (x->out.n || !x->st.running)
		tell(x);
	unlock(x);
}

void dmc_xfer_cancel(dmc_xfer *x)
{
	if (!x)
		return;
	lock(x);
	if (x->st.running && !x->cancelled) {
		/* what's queued dropped, what stops the other end in its place */
		buf_clear(&x->out);
		switch (x->protocol) {
		case DMC_XFER_KERMIT: {
			char e[128];
			int n = kermit_cancel_bytes(x, e, (int)sizeof(e));
			buf_add(&x->out, e, n);
			break;
		}
		case DMC_XFER_INTEL_HEX:
		case DMC_XFER_SREC:
		case DMC_XFER_TEXT:
			break;
		default:
			buf_add(&x->out, z_cancel, (int)sizeof(z_cancel));
			break;
		}
		x->cancelled = 1;
	}
	wake(x, 1);
	wake(x, 0);
	unlock(x);
}

void dmc_xfer_get_status(dmc_xfer *x, dmc_xfer_status *st)
{
	if (!x) {
		memset(st, 0, sizeof(*st));
		return;
	}
	lock(x);
	*st = x->st;
	unlock(x);
}

const char *dmc_xfer_received(dmc_xfer *x, int i)
{
	if (!x)
		return NULL;
	lock(x);
	const char *s = i >= 0 && i < x->nnames ? x->names[i] : NULL;
	unlock(x);
	return s;
}

int dmc_xfer_leftover(dmc_xfer *x, char *buf, int max)
{
	if (!x)
		return 0;
	lock(x);
	int n = 0;
	if (!x->st.running) {
		while (n < max && x->rpos < x->rlen)
			buf[n++] = (char)x->rbuf[x->rpos++];
		n += buf_take(&x->in, buf + n, max - n);
	}
	unlock(x);
	return n;
}

void dmc_xfer_free(dmc_xfer *x)
{
	if (!x)
		return;
	if (x->started) {
		dmc_xfer_cancel(x);
#ifdef _WIN32
		WaitForSingleObject(x->thread, INFINITE);
		CloseHandle(x->thread);
#else
		pthread_join(x->thread, NULL);
#endif
	}
#ifdef _WIN32
	DeleteCriticalSection(&x->cs);
	CloseHandle(x->ev_in);
	CloseHandle(x->ev_out);
#else
	pthread_mutex_destroy(&x->mu);
	pthread_cond_destroy(&x->cv_in);
	pthread_cond_destroy(&x->cv_out);
#endif
	buf_free(&x->in);
	buf_free(&x->out);
	for (int i = 0; i < x->nnames; i++)
		free(x->names[i]);
	free(x->names);
	for (int i = 0; i < x->nfiles; i++)
		free(x->files[i].path);
	free(x->files);
	free(x);
}

/* ------------------------------------------------------------------ */
/* what the remote starts */

static int find(const char *b, int n, const char *pat, int pn)
{
	for (int i = 0; i + pn <= n; i++)
		if (!memcmp(b + i, pat, (size_t)pn))
			return i;
	return -1;
}

int dmc_xfer_detect(const char *data, int n, int *at)
{
	int i = find(data, n, z_sz_start, (int)sizeof(z_sz_start));
	if (i >= 0) {
		if (at)
			*at = i;
		return DMC_XFER_DETECT_ZMODEM_SEND;
	}
	i = find(data, n, z_rz_start, (int)sizeof(z_rz_start));
	if (i >= 0) {
		if (at)
			*at = i;
		return DMC_XFER_DETECT_ZMODEM_RECEIVE;
	}
	/* Kermit's Send-Init: MARK, LEN, SEQ 0, S */
	for (i = 0; i + 3 < n; i++) {
		if (data[i] == 1 && data[i + 2] == ' ' && data[i + 3] == 'S') {
			int l = (unsigned char)data[i + 1] - 32;
			if (l >= 4 && l <= 94) {
				if (at)
					*at = i;
				return DMC_XFER_DETECT_KERMIT_SEND;
			}
		}
	}
	return DMC_XFER_DETECT_NONE;
}

/* ------------------------------------------------------------------ */
/* the checks */

unsigned xfer_crc16_byte(unsigned crc, int b)
{
	crc ^= (unsigned)(b & 0xff) << 8;
	for (int i = 0; i < 8; i++)
		crc = crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1;
	return crc & 0xffff;
}

unsigned xfer_crc16(unsigned crc, const unsigned char *p, int n)
{
	static unsigned table[256];
	static int ready;
	if (!ready) {
		for (int i = 0; i < 256; i++)
			table[i] = xfer_crc16_byte(0, i);
		ready = 1;
	}
	for (int i = 0; i < n; i++)
		crc = ((crc << 8) ^ table[((crc >> 8) ^ p[i]) & 0xff]) & 0xffff;
	return crc;
}

static unsigned long table32[256];

static void table32_make(void)
{
	if (table32[1])
		return;
	for (unsigned long i = 0; i < 256; i++) {
		unsigned long c = i;
		for (int k = 0; k < 8; k++)
			c = c & 1 ? (c >> 1) ^ 0xEDB88320UL : c >> 1;
		table32[i] = c;
	}
}

unsigned long xfer_crc32_byte(unsigned long crc, int b)
{
	table32_make();
	return (table32[(crc ^ (unsigned long)b) & 0xff] ^ (crc >> 8)) & 0xffffffffUL;
}

unsigned long xfer_crc32(unsigned long crc, const unsigned char *p, int n)
{
	table32_make();
	for (int i = 0; i < n; i++)
		crc = (table32[(crc ^ p[i]) & 0xff] ^ (crc >> 8)) & 0xffffffffUL;
	return crc;
}
