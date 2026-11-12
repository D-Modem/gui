/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * net.c - a remote login's protocol over a TCP connection the app makes:
 *         Telnet (RFC 854) and rlogin (RFC 1282).  Like the session, it
 *         does no I/O: the app hands it what the socket read
 *         (dmc_net_feed), writes what dmc_net_output gives, shows what
 *         dmc_net_screen gives and passes on what is typed (dmc_net_type).
 *
 * Telnet: the server's options answered (BINARY, ECHO and
 * SUPPRESS-GO-AHEAD taken; TERMINAL-TYPE and NAWS offered; the rest
 * refused), each option answered once per change so that no two ends
 * loop; IAC doubled both ways; a CR typed sent as CR NUL outside binary
 * mode; the window's size sent (NAWS) as it changes.
 *
 * rlogin: the handshake (NUL, the user twice, "term/speed"), the
 * server's NUL that answers it taken off the stream, and the window's
 * size sent ("\377\377ss" and four 16-bit numbers) once the server has
 * asked for it with the urgent byte 0x80.
 */
#include <stdlib.h>
#include <string.h>

#include "dmodemctl.h"
#include "util.h"

enum {
	IAC = 255, DONT = 254, DO = 253, WONT = 252, WILL = 251, SB = 250,
	SE = 240,
	O_BINARY = 0, O_ECHO = 1, O_SGA = 3, O_TTYPE = 24, O_NAWS = 31
};

/* where the Telnet reader is */
enum { T_DATA, T_IAC, T_OPT, T_SB, T_SB_IAC };

struct dmc_net {
	int proto;
	char *user, *term;
	int cols, rows;
	dmc_buf screen, out;
	/* Telnet */
	int st, verb, last_cr;
	unsigned char remote[256], local[256];
	dmc_buf sb;
	/* Telnet: the server asked for our size (DO NAWS) */
	int naws;
	/* rlogin: the server's NUL seen; it asked for the window's size */
	int ready, want_size;
};

DMC_API int dmc_net_default_port(int proto)
{
	switch (proto) {
	case DMC_NET_TELNET:
		return 23;
	case DMC_NET_RLOGIN:
		return 513;
	case DMC_NET_SSH:
		return 22;
	}
	return 0;
}

DMC_API const char *dmc_net_name(int proto)
{
	switch (proto) {
	case DMC_NET_TELNET:
		return "Telnet";
	case DMC_NET_RLOGIN:
		return "rlogin";
	case DMC_NET_SSH:
		return "SSH";
	}
	return "";
}

DMC_API dmc_net *dmc_net_new(int proto, const char *user, const char *term,
			     int cols, int rows)
{
	dmc_net *n = calloc(1, sizeof(*n));
	if (!n)
		return NULL;
	n->proto = proto;
	n->user = dup_str(user ? user : "");
	n->term = dup_str(term && *term ? term : "xterm");
	n->cols = cols;
	n->rows = rows;
	return n;
}

DMC_API void dmc_net_free(dmc_net *n)
{
	if (!n)
		return;
	free(n->user);
	free(n->term);
	buf_free(&n->screen);
	buf_free(&n->out);
	buf_free(&n->sb);
	free(n);
}

static void put(dmc_net *n, int a, int b, int c)
{
	char v[3];
	v[0] = (char)a;
	v[1] = (char)b;
	v[2] = (char)c;
	buf_add(&n->out, v, 3);
}

/* a 16-bit number, big-endian; for Telnet an 0xff in it doubled */
static void put16(dmc_net *n, int v, int telnet)
{
	int i;
	if (v < 0)
		v = 0;
	if (v > 65535)
		v = 65535;
	for (i = 0; i < 2; i++) {
		int b = i ? v & 0xff : v >> 8;
		buf_addc(&n->out, (char)b);
		if (telnet && b == IAC)
			buf_addc(&n->out, (char)IAC);
	}
}

static void send_naws(dmc_net *n)
{
	put(n, IAC, SB, O_NAWS);
	put16(n, n->cols, 1);
	put16(n, n->rows, 1);
	buf_addc(&n->out, (char)IAC);
	buf_addc(&n->out, (char)SE);
}

static void send_window(dmc_net *n)
{
	buf_add(&n->out, "\377\377ss", 4);
	put16(n, n->rows, 0);
	put16(n, n->cols, 0);
	put16(n, n->cols * 8, 0);
	put16(n, n->rows * 16, 0);
}

DMC_API void dmc_net_connected(dmc_net *n)
{
	if (n->proto == DMC_NET_RLOGIN) {
		/* NUL, the local and the remote user, "term/speed" */
		buf_addc(&n->out, '\0');
		buf_add(&n->out, n->user, (int)strlen(n->user) + 1);
		buf_add(&n->out, n->user, (int)strlen(n->user) + 1);
		buf_adds(&n->out, n->term);
		buf_add(&n->out, "/38400", 7);
	} else if (n->proto == DMC_NET_TELNET) {
		/* what a Telnet client offers at once: its type and size,
		   and no go-aheads; marked as asked, so that the server's
		   yes is not answered again */
		put(n, IAC, WILL, O_TTYPE);
		put(n, IAC, WILL, O_NAWS);
		put(n, IAC, DO, O_SGA);
		n->local[O_TTYPE] = n->local[O_NAWS] = 1;
		n->remote[O_SGA] = 1;
	}
}

/* the server's WILL, WONT, DO or DONT */
static void option(dmc_net *n, int verb, int opt)
{
	int ok;
	switch (verb) {
	case WILL:
		ok = opt == O_BINARY || opt == O_ECHO || opt == O_SGA;
		if (ok != n->remote[opt] || !ok) {
			n->remote[opt] = (unsigned char)ok;
			put(n, IAC, ok ? DO : DONT, opt);
		}
		break;
	case WONT:
		if (n->remote[opt]) {
			n->remote[opt] = 0;
			put(n, IAC, DONT, opt);
		}
		break;
	case DO:
		ok = opt == O_BINARY || opt == O_SGA || opt == O_TTYPE ||
			opt == O_NAWS;
		if (ok != n->local[opt] || !ok) {
			n->local[opt] = (unsigned char)ok;
			put(n, IAC, ok ? WILL : WONT, opt);
		}
		/* (its size, each time it asks) */
		if (ok && opt == O_NAWS) {
			n->naws = 1;
			send_naws(n);
		}
		break;
	case DONT:
		if (opt == O_NAWS)
			n->naws = 0;
		if (n->local[opt]) {
			n->local[opt] = 0;
			put(n, IAC, WONT, opt);
		}
		break;
	}
}

/* the server's subnegotiation: TERMINAL-TYPE SEND answered with IS */
static void subneg(dmc_net *n)
{
	const unsigned char *p = (const unsigned char *)n->sb.p;
	if (n->sb.n >= 2 && p[0] == O_TTYPE && p[1] == 1) {
		put(n, IAC, SB, O_TTYPE);
		buf_addc(&n->out, '\0');
		buf_adds(&n->out, n->term);
		buf_addc(&n->out, (char)IAC);
		buf_addc(&n->out, (char)SE);
	}
}

static void feed_telnet(dmc_net *n, const unsigned char *p, int len)
{
	int i;
	for (i = 0; i < len; i++) {
		int b = p[i];
		switch (n->st) {
		case T_DATA:
			if (b == IAC)
				n->st = T_IAC;
			/* CR NUL is a CR: the NUL goes */
			else if (!(b == 0 && n->last_cr && !n->remote[O_BINARY]))
				buf_addc(&n->screen, (char)b);
			break;
		case T_IAC:
			if (b == IAC) {
				buf_addc(&n->screen, (char)IAC);
				n->st = T_DATA;
			} else if (b == WILL || b == WONT || b == DO ||
				   b == DONT) {
				n->verb = b;
				n->st = T_OPT;
			} else if (b == SB) {
				buf_clear(&n->sb);
				n->st = T_SB;
			} else {
				/* NOP, GA, AYT and the rest: nothing to do */
				n->st = T_DATA;
			}
			break;
		case T_OPT:
			option(n, n->verb, b);
			n->st = T_DATA;
			break;
		case T_SB:
			if (b == IAC)
				n->st = T_SB_IAC;
			else if (n->sb.n < 1024)
				buf_addc(&n->sb, (char)b);
			break;
		case T_SB_IAC:
			if (b == SE) {
				subneg(n);
				n->st = T_DATA;
			} else {
				if (b == IAC && n->sb.n < 1024)
					buf_addc(&n->sb, (char)IAC);
				n->st = T_SB;
			}
			break;
		}
		n->last_cr = n->st == T_DATA && b == '\r';
	}
}

DMC_API void dmc_net_feed(dmc_net *n, const char *data, int len)
{
	if (len <= 0)
		return;
	if (n->proto == DMC_NET_TELNET) {
		feed_telnet(n, (const unsigned char *)data, len);
		return;
	}
	if (n->proto == DMC_NET_RLOGIN && !n->ready) {
		/* the server's NUL: the handshake taken */
		n->ready = 1;
		if (data[0] == '\0') {
			data++;
			len--;
		}
	}
	buf_add(&n->screen, data, len);
}

DMC_API void dmc_net_urgent(dmc_net *n, int byte)
{
	if (n->proto != DMC_NET_RLOGIN)
		return;
	/* TIOCPKT_WINDOW: the server takes window sizes, and wants ours */
	if (byte & 0x80) {
		n->want_size = 1;
		send_window(n);
	}
	/* TIOCPKT_FLUSHWRITE: what it sent before is to be dropped */
	if (byte & 0x02)
		buf_clear(&n->screen);
}

DMC_API int dmc_net_screen(dmc_net *n, char *buf, int max)
{
	return buf_take(&n->screen, buf, max);
}

DMC_API void dmc_net_type(dmc_net *n, const char *data, int len)
{
	int i;
	if (n->proto != DMC_NET_TELNET) {
		buf_add(&n->out, data, len);
		return;
	}
	for (i = 0; i < len; i++) {
		unsigned char b = (unsigned char)data[i];
		buf_addc(&n->out, (char)b);
		if (b == IAC)
			buf_addc(&n->out, (char)IAC);
		/* a lone CR is CR NUL, outside binary mode */
		if (b == '\r' && !n->local[O_BINARY] &&
		    (i + 1 >= len || data[i + 1] != '\n'))
			buf_addc(&n->out, '\0');
	}
}

DMC_API int dmc_net_output(dmc_net *n, char *buf, int max)
{
	return buf_take(&n->out, buf, max);
}

DMC_API void dmc_net_resize(dmc_net *n, int cols, int rows)
{
	if (cols == n->cols && rows == n->rows)
		return;
	n->cols = cols;
	n->rows = rows;
	if (n->proto == DMC_NET_TELNET && n->naws)
		send_naws(n);
	else if (n->proto == DMC_NET_RLOGIN && n->want_size)
		send_window(n);
}
