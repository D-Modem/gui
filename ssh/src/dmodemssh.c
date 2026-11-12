/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * dmodemssh.c - D-Modem's SSH client on libssh (see dmodemssh.h).
 *
 * The session is libssh's in its non-blocking mode: dmc_ssh_poll() runs
 * each step again until it no longer says "again" (connect, the login,
 * a channel's open and requests), reads every open channel, writes what
 * each has waiting as the server's window allows, and turns what
 * happened into events.  Between steps it waits on the socket with the
 * lock let go, so that what is typed goes straight out from the
 * caller's thread.
 *
 * This file and dmodemssh.h are the same in D-Modem/gui (ssh/) and
 * D-Modem/app (native-ssh/): change both.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
typedef CRITICAL_SECTION lock_t;
#define LOCK_INIT(l) InitializeCriticalSection(l)
#define LOCK_FREE(l) DeleteCriticalSection(l)
#define LOCK(l) EnterCriticalSection(l)
#define UNLOCK(l) LeaveCriticalSection(l)
/* ms milliseconds slept */
static void nap(int ms) { Sleep((DWORD)ms); }
#else
#include <pthread.h>
#include <sys/select.h>
#include <time.h>
typedef pthread_mutex_t lock_t;
#define LOCK_INIT(l) pthread_mutex_init(l, NULL)
#define LOCK_FREE(l) pthread_mutex_destroy(l)
#define LOCK(l) pthread_mutex_lock(l)
#define UNLOCK(l) pthread_mutex_unlock(l)
/* ms milliseconds slept */
static void nap(int ms)
{
	struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L };
	nanosleep(&t, NULL);
}
#endif

#include <libssh/libssh.h>
#include <libssh/callbacks.h>

#if LIBSSH_VERSION_INT < SSH_VERSION_INT(0, 11, 0)
#error libdmodemssh needs libssh 0.11 or later
#endif

#include "dmodemssh.h"
#include "ppk.h"
#include "agent.h"

static int import_key(const char *text, const char *passphrase, ssh_key *k);

#define MAX_CHANNELS 64
#define MAX_EVENTS 256
#define MAX_LISTENS 16
/* the longest the lock is let go while waiting on the socket */
#define SLICE_MS 25

/* the connection's steps */
enum {
	S_CONNECT, S_HOSTKEY, S_TRUST, S_NONE, S_CHOOSE, S_LOGIN,
	S_PROMPTS, S_READY, S_CLOSING, S_OVER
};

/* the login being tried */
enum { L_PASSWORD = 1, L_KEY, L_AGENT, L_AUTO, L_INTERACTIVE };

/* a channel's steps */
enum {
	C_FREE, C_OPEN_SESSION, C_OPEN_DIRECT, C_AGENT, C_PTY, C_SHELL,
	C_EXEC, C_OPEN, C_CLOSED
};

typedef struct {
	char *p;
	int len, cap;
} buf_t;

typedef struct {
	int step;
	ssh_channel ch;
	int agent;		/* ssh -A asked for this shell */
	char *term, *command, *host, *from;
	int cols, rows, port, from_port;
	buf_t out;		/* waiting for the server's window */
	int eof_out;		/* eof to send once out is empty */
	int eof_in, exit_told;
} chan_t;

typedef struct {
	int used, id, port;
	char *address;
	int pending;		/* the request still going */
} listen_t;

typedef struct {
	int type, channel, value;
	char *data;
	int len;
} ev_t;

struct dmc_ssh {
	lock_t lock;
	ssh_session ssh;
	ssh_event sev;
	struct ssh_callbacks_struct cb;
	char *user;
	int step;
	int known;		/* "knownhosts" given */
	char *hostkey;
	int trusted, remember;	/* the answer to S_HOSTKEY, -1 none yet */
	int methods;
	int login;		/* L_*, 0 none chosen */
	char *password;
	ssh_key key;
	char *why;		/* the reason the connection ended */
	int asked;		/* dmc_ssh_disconnect() */
	int answered;		/* a PROMPTS round's answers given */
	chan_t ch[MAX_CHANNELS];
	listen_t ls[MAX_LISTENS];
	int listens, next_listen;
	ev_t q[MAX_EVENTS];
	int qhead, qlen;
	ev_t cur;		/* the event last handed back */
	int over;		/* EV_CLOSED handed back */
	char *agent_which;	/* the "agent" option */
	dmc_agent *agent;	/* (Windows: what stands between) */
	int agent_set;		/* the agent looked for */
};

/* -- small things -- */

/* a copy of s, or NULL */
static char *copy(const char *s)
{
	char *d;
	size_t n;

	if (!s)
		return NULL;
	n = strlen(s) + 1;
	d = malloc(n);
	if (d)
		memcpy(d, s, n);
	return d;
}

/* len bytes added to b: 0, or -1 */
static int buf_add(buf_t *b, const char *p, int len)
{
	if (b->len + len > b->cap) {
		int cap = b->cap ? b->cap : 1024;
		char *n;

		while (cap < b->len + len)
			cap *= 2;
		n = realloc(b->p, (size_t)cap);
		if (!n)
			return -1;
		b->p = n;
		b->cap = cap;
	}
	memcpy(b->p + b->len, p, (size_t)len);
	b->len += len;
	return 0;
}

/* the first n bytes of b taken off */
static void buf_drop(buf_t *b, int n)
{
	memmove(b->p, b->p + n, (size_t)(b->len - n));
	b->len -= n;
}

/* an event queued (data copied, len bytes or a string when len < 0) */
static void post(dmc_ssh *s, int type, int channel, int value,
		 const char *data, int len)
{
	ev_t *e;

	if (s->qlen == MAX_EVENTS)
		return;
	e = &s->q[(s->qhead + s->qlen++) % MAX_EVENTS];
	e->type = type;
	e->channel = channel;
	e->value = value;
	e->data = NULL;
	e->len = 0;
	if (data) {
		if (len < 0)
			len = (int)strlen(data);
		e->data = malloc((size_t)len + 1);
		if (e->data) {
			memcpy(e->data, data, (size_t)len);
			e->data[len] = 0;
			e->len = len;
		}
	}
}

/* the connection ended, with why (NULL: asked) */
static void end(dmc_ssh *s, const char *why)
{
	if (s->step >= S_CLOSING)
		return;
	free(s->why);
	s->why = copy(why);
	s->step = S_CLOSING;
}

/* libssh's last error as why the connection ended */
static void failed(dmc_ssh *s, const char *what)
{
	char m[512];
	const char *e = ssh_get_error(s->ssh);

	snprintf(m, sizeof m, "%s%s%s", what, e && *e ? ": " : "", e ? e : "");
	end(s, m);
}

/* the channel number n's slot, or NULL */
static chan_t *slot(dmc_ssh *s, int n)
{
	if (n < 1 || n > MAX_CHANNELS || s->ch[n - 1].step == C_FREE)
		return NULL;
	return &s->ch[n - 1];
}

/* a free channel slot's number, or -1 */
static int new_slot(dmc_ssh *s)
{
	int i;

	if (s->step != S_READY)
		return -1;
	for (i = 0; i < MAX_CHANNELS; i++)
		if (s->ch[i].step == C_FREE) {
			memset(&s->ch[i], 0, sizeof s->ch[i]);
			s->ch[i].ch = ssh_channel_new(s->ssh);
			return s->ch[i].ch ? i + 1 : -1;
		}
	return -1;
}

/* a slot's things freed, the slot free */
static void free_slot(chan_t *c)
{
	if (c->ch)
		ssh_channel_free(c->ch);
	free(c->term);
	free(c->command);
	free(c->host);
	free(c->from);
	free(c->out.p);
	memset(c, 0, sizeof *c);
}

/* libssh's agent channel callback: the server opens one (ssh -A) */
static ssh_channel agent_channel(ssh_session ssh, void *user)
{
	dmc_ssh *s = user;
	int i;

	(void)ssh;
	for (i = 0; i < MAX_CHANNELS; i++)
		if (s->ch[i].step == C_FREE) {
			memset(&s->ch[i], 0, sizeof s->ch[i]);
			s->ch[i].ch = ssh_channel_new(s->ssh);
			if (!s->ch[i].ch)
				return NULL;
			s->ch[i].step = C_OPEN;
			post(s, DMC_SSH_EV_INCOMING, i + 1, DMC_SSH_IN_AGENT,
			     NULL, 0);
			return s->ch[i].ch;
		}
	return NULL;
}

/* -- the connection -- */

/* libssh initialised, once */
static void init_once(void)
{
	static int done;

	if (!done) {
		done = 1;
		ssh_init();
	}
}

/* a connection to make to user@host:port (nothing done until polled) */
dmc_ssh *dmc_ssh_new(const char *host, int port, const char *user)
{
	dmc_ssh *s;

	init_once();
	s = calloc(1, sizeof *s);
	if (!s)
		return NULL;
	s->ssh = ssh_new();
	if (!s->ssh) {
		free(s);
		return NULL;
	}
	LOCK_INIT(&s->lock);
	s->user = copy(user ? user : "");
	s->trusted = -1;
	ssh_options_set(s->ssh, SSH_OPTIONS_HOST, host);
	ssh_options_set(s->ssh, SSH_OPTIONS_PORT, &port);
	if (user && *user)
		ssh_options_set(s->ssh, SSH_OPTIONS_USER, user);
	{
		long t = 20;
		ssh_options_set(s->ssh, SSH_OPTIONS_TIMEOUT, &t);
	}
	/* (no known_hosts unless asked: the caller decides) */
	ssh_options_set(s->ssh, SSH_OPTIONS_KNOWNHOSTS, "/dev/null");
	ssh_options_set(s->ssh, SSH_OPTIONS_GLOBAL_KNOWNHOSTS, "/dev/null");
	ssh_callbacks_init(&s->cb);
	s->cb.userdata = s;
	s->cb.channel_open_request_auth_agent_function = agent_channel;
	ssh_set_callbacks(s->ssh, &s->cb);
	return s;
}

/* the connection dropped (if it's up) and freed */
void dmc_ssh_free(dmc_ssh *s)
{
	int i;

	if (!s)
		return;
	for (i = 0; i < MAX_CHANNELS; i++)
		if (s->ch[i].step != C_FREE)
			free_slot(&s->ch[i]);
	for (i = 0; i < MAX_LISTENS; i++)
		free(s->ls[i].address);
	if (s->sev)
		ssh_event_free(s->sev);
	if (ssh_is_connected(s->ssh))
		ssh_disconnect(s->ssh);
	ssh_free(s->ssh);
	dmc_agent_detach(s->agent);
	free(s->agent_which);
	if (s->key)
		ssh_key_free(s->key);
	while (s->qlen) {
		free(s->q[s->qhead].data);
		s->qhead = (s->qhead + 1) % MAX_EVENTS;
		s->qlen--;
	}
	free(s->cur.data);
	free(s->user);
	free(s->hostkey);
	if (s->password) {
		memset(s->password, 0, strlen(s->password));
		free(s->password);
	}
	free(s->why);
	LOCK_FREE(&s->lock);
	free(s);
}

/* an option, before the first poll: 0, or -1 if it's not one. "config"
   OpenSSH's config read (a path; "" its own ~/.ssh/config)
   "knownhosts"  the host's key looked up there (a path; "" OpenSSH's)
   "hostkeys"    the host key algorithms, in order (a list with commas)
   "timeout"     seconds to connect in "compression" "yes" or "no"
   "agent"       the agent logged in with (see dmc_ssh_agent_reachable) */
int dmc_ssh_option(dmc_ssh *s, const char *name, const char *value)
{
	int r = -1;

	LOCK(&s->lock);
	if (!strcmp(name, "config")) {
		r = ssh_options_parse_config(s->ssh, *value ? value : NULL);
	} else if (!strcmp(name, "knownhosts")) {
		if (*value) {
			r = ssh_options_set(s->ssh, SSH_OPTIONS_KNOWNHOSTS, value);
		} else {
			/* (OpenSSH's own, and the system's) */
			r = ssh_options_set(s->ssh, SSH_OPTIONS_KNOWNHOSTS, NULL);
			ssh_options_set(s->ssh, SSH_OPTIONS_GLOBAL_KNOWNHOSTS, NULL);
		}
		s->known = r == 0;
	} else if (!strcmp(name, "hostkeys")) {
		r = ssh_options_set(s->ssh, SSH_OPTIONS_HOSTKEYS, value);
	} else if (!strcmp(name, "timeout")) {
		long t = atol(value);
		r = ssh_options_set(s->ssh, SSH_OPTIONS_TIMEOUT, &t);
	} else if (!strcmp(name, "compression")) {
		r = ssh_options_set(s->ssh, SSH_OPTIONS_COMPRESSION, value);
	} else if (!strcmp(name, "agent")) {
		free(s->agent_which);
		s->agent_which = copy(value);
		r = s->agent_which ? 0 : -1;
	}
	UNLOCK(&s->lock);
	return r == 0 ? 0 : -1;
}

/* the server's key as its OpenSSH line, kept; and how known_hosts has it */
static int read_hostkey(dmc_ssh *s)
{
	ssh_key k = NULL;
	char *b64 = NULL;
	const char *type;
	size_t n;
	int known = DMC_SSH_HOST_UNCHECKED;

	if (ssh_get_server_publickey(s->ssh, &k) != SSH_OK)
		return -1;
	type = ssh_key_type_to_char(ssh_key_type(k));
	if (ssh_pki_export_pubkey_base64(k, &b64) != SSH_OK || !type) {
		ssh_key_free(k);
		return -1;
	}
	n = strlen(type) + strlen(b64) + 2;
	s->hostkey = malloc(n);
	if (s->hostkey)
		snprintf(s->hostkey, n, "%s %s", type, b64);
	ssh_string_free_char(b64);
	ssh_key_free(k);
	if (!s->hostkey)
		return -1;
	if (s->known) {
		switch (ssh_session_is_known_server(s->ssh)) {
		case SSH_KNOWN_HOSTS_OK: known = DMC_SSH_HOST_KNOWN; break;
		case SSH_KNOWN_HOSTS_CHANGED: known = DMC_SSH_HOST_CHANGED; break;
		case SSH_KNOWN_HOSTS_OTHER: known = DMC_SSH_HOST_OTHER_TYPE; break;
		default: known = DMC_SSH_HOST_NEW; break;
		}
	}
	post(s, DMC_SSH_EV_HOSTKEY, 0, known, s->hostkey, -1);
	return 0;
}

/* the methods the server takes, as DMC_SSH_AUTH_* */
static int methods(dmc_ssh *s)
{
	int m = ssh_userauth_list(s->ssh, NULL), r = 0;

	if (m & SSH_AUTH_METHOD_PASSWORD)
		r |= DMC_SSH_AUTH_PASSWORD;
	if (m & SSH_AUTH_METHOD_PUBLICKEY)
		r |= DMC_SSH_AUTH_PUBLICKEY;
	if (m & SSH_AUTH_METHOD_INTERACTIVE)
		r |= DMC_SSH_AUTH_INTERACTIVE;
	return r;
}

/* logged in: channels may open */
static void ready(dmc_ssh *s)
{
	char *banner = ssh_get_issue_banner(s->ssh);

	s->step = S_READY;
	s->sev = ssh_event_new();
	if (s->sev)
		ssh_event_add_session(s->sev, s->ssh);
	post(s, DMC_SSH_EV_READY, 0, 0, banner, -1);
	if (banner)
		ssh_string_free_char(banner);
}

/* a login's result acted on */
static void login_result(dmc_ssh *s, int r)
{
	const char *why = "";

	switch (r) {
	case SSH_AUTH_AGAIN:
		return;
	case SSH_AUTH_SUCCESS:
		ready(s);
		return;
	case SSH_AUTH_INFO:
		s->answered = 0;
		s->step = S_PROMPTS;
		post(s, DMC_SSH_EV_PROMPTS, 0,
		     ssh_userauth_kbdint_getnprompts(s->ssh),
		     ssh_userauth_kbdint_getname(s->ssh), -1);
		return;
	case SSH_AUTH_PARTIAL:
		why = "partial";
		break;
	case SSH_AUTH_DENIED:
		why = s->login == L_PASSWORD ? "The password was not accepted" :
		      s->login == L_INTERACTIVE ? "The answers were not accepted" :
		      "The key was not accepted";
		break;
	default:
		failed(s, "the login failed");
		return;
	}
	s->login = 0;
	s->methods = methods(s);
	s->step = S_CHOOSE;
	post(s, DMC_SSH_EV_AUTH, 0, s->methods, why, -1);
}

/* the agent found, once, for the logins that use it */
static void agent_setup(dmc_ssh *s)
{
	if (s->agent_set)
		return;
	s->agent_set = 1;
	dmc_agent_attach(s->ssh, s->agent_which, &s->agent);
}

/* the login chosen, tried again until it's done */
static void login(dmc_ssh *s)
{
	int r = SSH_AUTH_ERROR;

	switch (s->login) {
	case L_PASSWORD:
		r = ssh_userauth_password(s->ssh, NULL, s->password);
		break;
	case L_KEY:
		r = ssh_userauth_publickey(s->ssh, NULL, s->key);
		break;
	case L_AGENT:
		agent_setup(s);
		r = ssh_userauth_agent(s->ssh, NULL);
		break;
	case L_AUTO:
		agent_setup(s);
		r = ssh_userauth_publickey_auto(s->ssh, NULL, NULL);
		break;
	case L_INTERACTIVE:
		r = ssh_userauth_kbdint(s->ssh, NULL, NULL);
		break;
	}
	if (r != SSH_AUTH_AGAIN && s->password && s->login == L_PASSWORD) {
		memset(s->password, 0, strlen(s->password));
		free(s->password);
		s->password = NULL;
	}
	login_result(s, r);
}

/* a channel's open and requests, each run again until it's done */
static void channel_step(dmc_ssh *s, int n)
{
	chan_t *c = &s->ch[n - 1];
	int r;

	for (;;) {
		switch (c->step) {
		case C_OPEN_SESSION:
			r = ssh_channel_open_session(c->ch);
			if (r == SSH_AGAIN)
				return;
			if (r != SSH_OK)
				goto refused;
			c->step = c->agent ? C_AGENT : c->command ? C_EXEC : C_PTY;
			continue;
		case C_OPEN_DIRECT:
			r = ssh_channel_open_forward(c->ch, c->host, c->port,
						     c->from ? c->from : "127.0.0.1",
						     c->from_port);
			if (r == SSH_AGAIN)
				return;
			if (r != SSH_OK)
				goto refused;
			c->step = C_OPEN;
			post(s, DMC_SSH_EV_OPEN, n, 1, NULL, 0);
			return;
		case C_AGENT:
			/* (refused: the shell goes on without it) */
			r = ssh_channel_request_auth_agent(c->ch);
			if (r == SSH_AGAIN)
				return;
			c->step = c->command ? C_EXEC : C_PTY;
			continue;
		case C_PTY:
			r = ssh_channel_request_pty_size(c->ch, c->term, c->cols, c->rows);
			if (r == SSH_AGAIN)
				return;
			if (r != SSH_OK)
				goto refused;
			c->step = C_SHELL;
			continue;
		case C_SHELL:
			r = ssh_channel_request_shell(c->ch);
			if (r == SSH_AGAIN)
				return;
			if (r != SSH_OK)
				goto refused;
			c->step = C_OPEN;
			post(s, DMC_SSH_EV_OPEN, n, 1, NULL, 0);
			return;
		case C_EXEC:
			r = ssh_channel_request_exec(c->ch, c->command);
			if (r == SSH_AGAIN)
				return;
			if (r != SSH_OK)
				goto refused;
			c->step = C_OPEN;
			post(s, DMC_SSH_EV_OPEN, n, 1, NULL, 0);
			return;
		default:
			return;
		}
	}
refused:
	post(s, DMC_SSH_EV_OPEN, n, 0, ssh_get_error(s->ssh), -1);
	post(s, DMC_SSH_EV_CLOSE, n, 0, NULL, 0);
	c->step = C_CLOSED;
}

/* what a channel has waiting written, as the server's window allows */
static void channel_flush(dmc_ssh *s, chan_t *c)
{
	(void)s;
	while (c->out.len > 0) {
		uint32_t w = ssh_channel_window_size(c->ch);
		int n = c->out.len, r;

		if (w == 0)
			return;
		if ((uint32_t)n > w)
			n = (int)w;
		r = ssh_channel_write(c->ch, c->out.p, (uint32_t)n);
		if (r <= 0)
			return;
		buf_drop(&c->out, r);
	}
	if (c->eof_out == 1) {
		ssh_channel_send_eof(c->ch);
		c->eof_out = 2;
	}
}

/* an open channel read: its data, end, exit and close as events */
static void chan_read(dmc_ssh *s, int n)
{
	chan_t *c = &s->ch[n - 1];
	char b[16384];
	int err, r, got = 1;

	channel_flush(s, c);
	while (got) {
		got = 0;
		for (err = 0; err < 2; err++) {
			r = ssh_channel_read_nonblocking(c->ch, b, sizeof b, err);
			if (r > 0) {
				post(s, DMC_SSH_EV_DATA, n, err, b, r);
				got = s->qlen < MAX_EVENTS - 8;
			}
		}
	}
	if (!c->eof_in && ssh_channel_is_eof(c->ch) &&
	    ssh_channel_poll(c->ch, 0) <= 0 && ssh_channel_poll(c->ch, 1) <= 0) {
		c->eof_in = 1;
		post(s, DMC_SSH_EV_EOF, n, 0, NULL, 0);
	}
	if (!c->exit_told) {
		uint32_t code = 0;
		char *sig = NULL;
		int core = 0;

		if (ssh_channel_get_exit_state(c->ch, &code, &sig, &core) == SSH_OK &&
		    (code != (uint32_t)-1 || sig)) {
			c->exit_told = 1;
			post(s, DMC_SSH_EV_EXIT, n, (int)code, sig, -1);
			if (sig)
				ssh_string_free_char(sig);
		}
	}
	if (ssh_channel_is_closed(c->ch) &&
	    ssh_channel_poll(c->ch, 0) <= 0 && ssh_channel_poll(c->ch, 1) <= 0) {
		c->step = C_CLOSED;
		post(s, DMC_SSH_EV_CLOSE, n, 0, NULL, 0);
	}
}

/* the remote listens' requests run on, and their connections taken */
static void listens(dmc_ssh *s)
{
	int i, r, port, from_port = 0;
	char *from = NULL;
	ssh_channel ch;

	for (i = 0; i < MAX_LISTENS; i++) {
		listen_t *l = &s->ls[i];
		int bound = 0;

		if (!l->used || !l->pending)
			continue;
		r = ssh_channel_listen_forward(s->ssh, l->address, l->port, &bound);
		if (r == SSH_AGAIN)
			continue;
		l->pending = 0;
		if (r == SSH_OK) {
			if (l->port == 0)
				l->port = bound;
			post(s, DMC_SSH_EV_LISTEN, l->id, l->port, NULL, 0);
		} else {
			post(s, DMC_SSH_EV_LISTEN, l->id, -1, ssh_get_error(s->ssh), -1);
			free(l->address);
			memset(l, 0, sizeof *l);
			s->listens--;
		}
	}
	if (!s->listens)
		return;
	while ((ch = ssh_channel_open_forward_port(s->ssh, 0, &port, &from,
						   &from_port)) != NULL) {
		int n = -1;

		for (i = 0; i < MAX_CHANNELS; i++)
			if (s->ch[i].step == C_FREE) {
				n = i + 1;
				break;
			}
		if (n < 0) {
			ssh_channel_close(ch);
			ssh_channel_free(ch);
		} else {
			memset(&s->ch[n - 1], 0, sizeof s->ch[n - 1]);
			s->ch[n - 1].ch = ch;
			s->ch[n - 1].step = C_OPEN;
			post(s, DMC_SSH_EV_INCOMING, n, port, from, -1);
		}
		if (from)
			ssh_string_free_char(from);
		from = NULL;
	}
}

/* every step that can go on, gone on with; events queued */
static void run(dmc_ssh *s)
{
	int r, i;

	switch (s->step) {
	case S_CONNECT:
		ssh_set_blocking(s->ssh, 0);
		r = ssh_connect(s->ssh);
		if (r == SSH_AGAIN)
			return;
		if (r != SSH_OK) {
			failed(s, "could not connect");
			return;
		}
		s->step = S_HOSTKEY;
		/* fall through */
	case S_HOSTKEY:
		if (read_hostkey(s) < 0) {
			failed(s, "no host key");
			return;
		}
		s->step = S_TRUST;
		/* fall through */
	case S_TRUST:
		if (s->trusted < 0)
			return;
		if (!s->trusted) {
			end(s, "the host's key was not accepted");
			return;
		}
		if (s->remember && s->known)
			ssh_session_update_known_hosts(s->ssh);
		s->step = S_NONE;
		/* fall through */
	case S_NONE:
		r = ssh_userauth_none(s->ssh, NULL);
		if (r == SSH_AUTH_AGAIN)
			return;
		if (r == SSH_AUTH_SUCCESS) {
			ready(s);
			return;
		}
		if (r == SSH_AUTH_ERROR) {
			failed(s, "the login failed");
			return;
		}
		s->methods = methods(s);
		s->step = S_CHOOSE;
		post(s, DMC_SSH_EV_AUTH, 0, s->methods, "", 0);
		return;
	case S_CHOOSE:
		if (!s->login)
			return;
		s->step = S_LOGIN;
		/* fall through */
	case S_LOGIN:
		login(s);
		return;
	case S_PROMPTS:
		if (!s->answered)
			return;
		s->step = S_LOGIN;
		login(s);
		return;
	case S_READY:
		if (s->sev)
			ssh_event_dopoll(s->sev, 0);
		if (!ssh_is_connected(s->ssh)) {
			const char *m = ssh_get_disconnect_message(s->ssh);
			end(s, m && *m ? m : "Connection closed by the server");
			return;
		}
		listens(s);
		for (i = 0; i < MAX_CHANNELS; i++) {
			int st = s->ch[i].step;

			if (st == C_OPEN)
				chan_read(s, i + 1);
			else if (st != C_FREE && st != C_CLOSED)
				channel_step(s, i + 1);
		}
		return;
	case S_CLOSING:
		for (i = 0; i < MAX_CHANNELS; i++)
			if (s->ch[i].step != C_FREE)
				free_slot(&s->ch[i]);
		if (ssh_is_connected(s->ssh))
			ssh_disconnect(s->ssh);
		post(s, DMC_SSH_EV_CLOSED, 0, 0, s->asked ? NULL : s->why, -1);
		s->step = S_OVER;
		return;
	}
}

/* the socket's file descriptor, if there is one */
static socket_t sock(dmc_ssh *s)
{
	return s->step < S_CLOSING ? ssh_get_fd(s->ssh) : SSH_INVALID_SOCKET;
}

/* up to ms waited for the socket to have something (the lock let go) */
static void wait_socket(socket_t fd, int ms)
{
	fd_set r;
	struct timeval t;

	if (fd == SSH_INVALID_SOCKET) {
		nap(ms);
		return;
	}
	FD_ZERO(&r);
	FD_SET(fd, &r);
	t.tv_sec = ms / 1000;
	t.tv_usec = (ms % 1000) * 1000;
	select((int)fd + 1, &r, NULL, NULL, &t);
}

/* the next event: 1 (in *ev), 0 when timeout_ms passed with none, -1
   when the connection is over and its DMC_SSH_EV_CLOSED was handed
   back */
int dmc_ssh_poll(dmc_ssh *s, int timeout_ms, dmc_ssh_event *ev)
{
	int waited = 0;

	LOCK(&s->lock);
	free(s->cur.data);
	s->cur.data = NULL;
	for (;;) {
		socket_t fd;
		int ms;

		if (s->over) {
			UNLOCK(&s->lock);
			return -1;
		}
		if (!s->qlen && s->step != S_OVER)
			run(s);
		if (s->qlen) {
			s->cur = s->q[s->qhead];
			s->qhead = (s->qhead + 1) % MAX_EVENTS;
			s->qlen--;
			ev->type = s->cur.type;
			ev->channel = s->cur.channel;
			ev->value = s->cur.value;
			ev->data = s->cur.data;
			ev->len = s->cur.len;
			if (ev->type == DMC_SSH_EV_CLOSED)
				s->over = 1;
			/* (a closed channel's number is free once that's told) */
			if (ev->type == DMC_SSH_EV_CLOSE && ev->channel > 0 &&
			    s->ch[ev->channel - 1].step == C_CLOSED)
				free_slot(&s->ch[ev->channel - 1]);
			UNLOCK(&s->lock);
			return 1;
		}
		if (waited >= timeout_ms) {
			UNLOCK(&s->lock);
			return 0;
		}
		fd = sock(s);
		UNLOCK(&s->lock);
		ms = timeout_ms - waited < SLICE_MS ? timeout_ms - waited : SLICE_MS;
		wait_socket(fd, ms);
		waited += ms;
		LOCK(&s->lock);
	}
}

/* the connection closed by the user: a DMC_SSH_EV_CLOSED with NULL
   comes */
void dmc_ssh_disconnect(dmc_ssh *s)
{
	LOCK(&s->lock);
	s->asked = 1;
	end(s, NULL);
	UNLOCK(&s->lock);
}

/* -- the login -- */

/* the server's key: trusted (ok) or not; remember: added to
   known_hosts */
void dmc_ssh_trust(dmc_ssh *s, int ok, int remember)
{
	LOCK(&s->lock);
	s->trusted = ok != 0;
	s->remember = remember != 0;
	UNLOCK(&s->lock);
}

/* login chosen, if one is wanted */
static void choose(dmc_ssh *s, int how)
{
	if (s->step == S_CHOOSE && !s->login)
		s->login = how;
}

/* a login tried (an answer to DMC_SSH_EV_AUTH) */
void dmc_ssh_auth_password(dmc_ssh *s, const char *password)
{
	LOCK(&s->lock);
	if (s->step == S_CHOOSE && !s->login) {
		free(s->password);
		s->password = copy(password ? password : "");
		choose(s, L_PASSWORD);
	}
	UNLOCK(&s->lock);
}

/* by a private key's text (OpenSSH's, PEM or PuTTY's .ppk): 0, or -1 when it can't be
   read with that passphrase (nothing is tried: answer again) */
int dmc_ssh_auth_key(dmc_ssh *s, const char *key, const char *passphrase)
{
	ssh_key k = NULL;

	if (import_key(key, passphrase, &k))
		return -1;
	LOCK(&s->lock);
	if (s->step == S_CHOOSE && !s->login) {
		if (s->key)
			ssh_key_free(s->key);
		s->key = k;
		k = NULL;
		choose(s, L_KEY);
	}
	UNLOCK(&s->lock);
	if (k)
		ssh_key_free(k);
	return 0;
}

/* by the agent: the "agent" option's, else the system's (SSH_AUTH_SOCK;
   on Windows Pageant, else OpenSSH's) */
void dmc_ssh_auth_agent(dmc_ssh *s)
{
	LOCK(&s->lock);
	choose(s, L_AGENT);
	UNLOCK(&s->lock);
}

/* whether the agent answers (see dmodemssh.h): 1 or 0 */
int dmc_ssh_agent_reachable(const char *agent)
{
	return dmc_agent_reachable(agent);
}

/* as OpenSSH's ssh does: the agent, then ~/.ssh's keys with no
   passphrase */
void dmc_ssh_auth_auto(dmc_ssh *s)
{
	LOCK(&s->lock);
	choose(s, L_AUTO);
	UNLOCK(&s->lock);
}

/* by the server's questions (keyboard-interactive) */
void dmc_ssh_auth_interactive(dmc_ssh *s)
{
	LOCK(&s->lock);
	choose(s, L_INTERACTIVE);
	UNLOCK(&s->lock);
}

/* no more tries: the connection closes, "not logged in" */
void dmc_ssh_auth_cancel(dmc_ssh *s)
{
	LOCK(&s->lock);
	if (s->step < S_READY)
		end(s, "not logged in");
	UNLOCK(&s->lock);
}

/* a DMC_SSH_EV_PROMPTS round's instruction */
const char *dmc_ssh_instruction(dmc_ssh *s)
{
	const char *r;

	LOCK(&s->lock);
	r = s->step == S_PROMPTS ? ssh_userauth_kbdint_getinstruction(s->ssh) : NULL;
	UNLOCK(&s->lock);
	return r ? r : "";
}

/* a DMC_SSH_EV_PROMPTS round: its instruction, and question i (*echo:
   whether the answer may be shown) */
const char *dmc_ssh_prompt(dmc_ssh *s, int i, int *echo)
{
	const char *r = NULL;
	char e = 0;

	LOCK(&s->lock);
	if (s->step == S_PROMPTS && i >= 0)
		r = ssh_userauth_kbdint_getprompt(s->ssh, (unsigned)i, &e);
	UNLOCK(&s->lock);
	if (echo)
		*echo = e != 0;
	return r ? r : "";
}

/* question i's answer, for the round's dmc_ssh_answered() */
void dmc_ssh_answer(dmc_ssh *s, int i, const char *answer)
{
	LOCK(&s->lock);
	if (s->step == S_PROMPTS && i >= 0)
		ssh_userauth_kbdint_setanswer(s->ssh, (unsigned)i, answer ? answer : "");
	UNLOCK(&s->lock);
}

/* the round's answers sent */
void dmc_ssh_answered(dmc_ssh *s)
{
	LOCK(&s->lock);
	if (s->step == S_PROMPTS)
		s->answered = 1;
	UNLOCK(&s->lock);
}

/* -- channels -- */

/* a shell on a pty of term in the given size; agent: ssh -A asked */
int dmc_ssh_shell(dmc_ssh *s, const char *term, int cols, int rows, int agent)
{
	int n;

	LOCK(&s->lock);
	n = new_slot(s);
	if (n > 0) {
		chan_t *c = &s->ch[n - 1];
		c->term = copy(term && *term ? term : "xterm-256color");
		c->cols = cols;
		c->rows = rows;
		c->agent = agent;
		c->step = C_OPEN_SESSION;
	}
	UNLOCK(&s->lock);
	return n;
}

/* a command run, with no pty; agent: ssh -A asked */
int dmc_ssh_exec(dmc_ssh *s, const char *command, int agent)
{
	int n;

	LOCK(&s->lock);
	n = new_slot(s);
	if (n > 0) {
		s->ch[n - 1].command = copy(command);
		s->ch[n - 1].agent = agent;
		s->ch[n - 1].step = C_OPEN_SESSION;
	}
	UNLOCK(&s->lock);
	return n;
}

/* a TCP connection made from the server to host:port (ssh -L, -D), for
   a connection that came from from_host:from_port */
int dmc_ssh_direct(dmc_ssh *s, const char *host, int port,
		   const char *from_host, int from_port)
{
	int n;

	LOCK(&s->lock);
	n = new_slot(s);
	if (n > 0) {
		chan_t *c = &s->ch[n - 1];
		c->host = copy(host);
		c->port = port;
		c->from = copy(from_host);
		c->from_port = from_port;
		c->step = C_OPEN_DIRECT;
	}
	UNLOCK(&s->lock);
	return n;
}

/* the server asked to listen on address:port (ssh -R; NULL: its
   default, 0: a port it picks): a request's number, answered by a
   DMC_SSH_EV_LISTEN; its connections come as DMC_SSH_EV_INCOMING */
int dmc_ssh_listen(dmc_ssh *s, const char *address, int port)
{
	int i, id = -1;

	LOCK(&s->lock);
	if (s->step == S_READY)
		for (i = 0; i < MAX_LISTENS; i++)
			if (!s->ls[i].used) {
				listen_t *l = &s->ls[i];
				l->used = 1;
				l->pending = 1;
				l->id = id = ++s->next_listen;
				l->port = port;
				l->address = copy(address);
				s->listens++;
				break;
			}
	UNLOCK(&s->lock);
	return id;
}

/* that listen stopped */
void dmc_ssh_unlisten(dmc_ssh *s, const char *address, int port)
{
	int i;

	LOCK(&s->lock);
	for (i = 0; i < MAX_LISTENS; i++) {
		listen_t *l = &s->ls[i];

		if (!l->used || l->port != port ||
		    !(address ? l->address && !strcmp(address, l->address) : !l->address))
			continue;
		if (s->step == S_READY && !l->pending) {
			/* (in blocking mode, briefly: it's one request) */
			ssh_set_blocking(s->ssh, 1);
			ssh_channel_cancel_forward(s->ssh, l->address, l->port);
			ssh_set_blocking(s->ssh, 0);
		}
		free(l->address);
		memset(l, 0, sizeof *l);
		s->listens--;
	}
	UNLOCK(&s->lock);
}

/* data to a channel (kept until the server can take it): how many
   bytes are still kept (0 when all went; len 0 asks), or -1 if the
   channel isn't open */
int dmc_ssh_write(dmc_ssh *s, int channel, const char *data, int len)
{
	chan_t *c;
	int r = -1;

	LOCK(&s->lock);
	c = slot(s, channel);
	if (c && c->step == C_OPEN && !c->eof_out && s->step == S_READY &&
	    buf_add(&c->out, data, len) == 0) {
		channel_flush(s, c);
		r = c->out.len;
	}
	UNLOCK(&s->lock);
	return r;
}

/* the channel's end of data sent */
void dmc_ssh_eof(dmc_ssh *s, int channel)
{
	chan_t *c;

	LOCK(&s->lock);
	c = slot(s, channel);
	if (c && c->step == C_OPEN && !c->eof_out) {
		c->eof_out = 1;
		channel_flush(s, c);
	}
	UNLOCK(&s->lock);
}

/* the channel closed (a DMC_SSH_EV_CLOSE comes) */
void dmc_ssh_close(dmc_ssh *s, int channel)
{
	chan_t *c;

	LOCK(&s->lock);
	c = slot(s, channel);
	if (c && c->step != C_CLOSED) {
		if (c->step == C_OPEN)
			ssh_channel_close(c->ch);
		c->step = C_CLOSED;
		post(s, DMC_SSH_EV_CLOSE, channel, 0, NULL, 0);
	}
	UNLOCK(&s->lock);
}

/* a shell's pty resized */
void dmc_ssh_resize(dmc_ssh *s, int channel, int cols, int rows)
{
	chan_t *c;

	LOCK(&s->lock);
	c = slot(s, channel);
	if (c && c->term) {
		c->cols = cols;
		c->rows = rows;
		if (c->step == C_OPEN)
			ssh_channel_change_pty_size(c->ch, cols, rows);
	}
	UNLOCK(&s->lock);
}

/* -- keys -- */

/* a private key's text read: OpenSSH's, PEM or PuTTY's .ppk, with its
   passphrase (NULL or "": none): 0, or -1 */
static int import_key(const char *text, const char *passphrase, ssh_key *k)
{
	const char *pp = passphrase && *passphrase ? passphrase : NULL;
	char *openssh = NULL;
	int r;

	*k = NULL;
	if (!text)
		return -1;
	if (!dmc_ppk_is(text))
		return ssh_pki_import_privkey_base64(text, pp, NULL, NULL, k) == SSH_OK ? 0 : -1;
	if (dmc_ppk_to_openssh(text, pp, &openssh))
		return -1;
	r = ssh_pki_import_privkey_base64(openssh, NULL, NULL, NULL, k) == SSH_OK ? 0 : -1;
	memset(openssh, 0, strlen(openssh));
	free(openssh);
	return r;
}

/* "type base64" of a public key, malloc'd, or NULL */
static char *pub_line(ssh_key k)
{
	char *b64 = NULL, *r;
	const char *type = ssh_key_type_to_char(ssh_key_type(k));
	size_t n;

	if (!type || ssh_pki_export_pubkey_base64(k, &b64) != SSH_OK)
		return NULL;
	n = strlen(type) + strlen(b64) + 2;
	r = malloc(n);
	if (r)
		snprintf(r, n, "%s %s", type, b64);
	ssh_string_free_char(b64);
	return r;
}

/* OpenSSH's SHA256 fingerprint of a key's OpenSSH line ("SHA256:..."),
   into buf: 0, or -1 if it isn't a key */
int dmc_ssh_fingerprint(const char *keyline, char *buf, int max)
{
	char type[64], *b64;
	const char *sp = strchr(keyline, ' ');
	ssh_key k = NULL;
	unsigned char *h = NULL;
	size_t hl = 0;
	char *f;
	int r = -1;

	init_once();
	if (!sp || sp - keyline >= (int)sizeof type)
		return -1;
	memcpy(type, keyline, (size_t)(sp - keyline));
	type[sp - keyline] = 0;
	b64 = copy(sp + 1);
	if (!b64)
		return -1;
	b64[strcspn(b64, " \r\n")] = 0;
	if (ssh_pki_import_pubkey_base64(b64, ssh_key_type_from_name(type), &k) == SSH_OK &&
	    ssh_get_publickey_hash(k, SSH_PUBLICKEY_HASH_SHA256, &h, &hl) == 0) {
		f = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, h, hl);
		if (f) {
			snprintf(buf, (size_t)max, "%s", f);
			ssh_string_free_char(f);
			r = 0;
		}
		ssh_clean_pubkey_hash(&h);
	}
	if (k)
		ssh_key_free(k);
	free(b64);
	return r;
}

/* a private key exported in OpenSSH's form, malloc'd */
static int export_key(ssh_key k, const char *passphrase, char **out)
{
	char *b64 = NULL;

	const char *pp = passphrase && *passphrase ? passphrase : NULL;

	if (ssh_pki_export_privkey_base64_format(k, pp, NULL, NULL, &b64,
						 SSH_FILE_FORMAT_OPENSSH) != SSH_OK)
		return -1;
	*out = copy(b64);
	ssh_string_free_char(b64);
	return *out ? 0 : -1;
}

/* a new key ("ed25519", "ecdsa" with bits 256, 384 or 521, "rsa" with
   bits): its private text (OpenSSH's, encrypted with passphrase unless
   it's NULL or "") and its public line, both to free with
   dmc_ssh_free_string(): 0, or -1 */
int dmc_ssh_keygen(const char *type, int bits, const char *passphrase,
		   char **private_key, char **public_key)
{
	enum ssh_keytypes_e t;
	ssh_key k = NULL;
	int r = -1;

	init_once();
	*private_key = *public_key = NULL;
	if (!strcmp(type, "ed25519")) {
		t = SSH_KEYTYPE_ED25519;
		bits = 0;
	} else if (!strcmp(type, "ecdsa")) {
		t = bits == 521 ? SSH_KEYTYPE_ECDSA_P521 :
		    bits == 384 ? SSH_KEYTYPE_ECDSA_P384 : SSH_KEYTYPE_ECDSA_P256;
	} else if (!strcmp(type, "rsa")) {
		t = SSH_KEYTYPE_RSA;
		if (bits < 2048)
			bits = 3072;
	} else {
		return -1;
	}
	if (ssh_pki_generate(t, bits, &k) != SSH_OK)
		return -1;
	if (export_key(k, passphrase, private_key) == 0) {
		*public_key = pub_line(k);
		r = *public_key ? 0 : -1;
	}
	ssh_key_free(k);
	if (r < 0) {
		free(*private_key);
		*private_key = NULL;
	}
	return r;
}

/* a private key's public line: 0, or -1 if it can't be read with that
   passphrase */
int dmc_ssh_key_public(const char *key, const char *passphrase, char **public_key)
{
	ssh_key k = NULL;

	init_once();
	*public_key = NULL;
	if (import_key(key, passphrase, &k))
		return -1;
	*public_key = pub_line(k);
	ssh_key_free(k);
	return *public_key ? 0 : -1;
}

/* a private key written again (OpenSSH's form) with a new passphrase
   (NULL or "": none): 0, or -1 */
int dmc_ssh_key_export(const char *key, const char *passphrase,
		       const char *new_passphrase, char **out)
{
	ssh_key k = NULL;
	int r;

	init_once();
	*out = NULL;
	if (import_key(key, passphrase, &k))
		return -1;
	r = export_key(k, new_passphrase, out);
	ssh_key_free(k);
	return r;
}

/* whether a private key's text needs a passphrase (OpenSSH's, PEM or
   PuTTY's .ppk): 1 or 0 */
int dmc_ssh_key_encrypted(const char *key)
{
	return dmc_key_text_encrypted(key);
}

/* a string these functions gave, freed */
void dmc_ssh_free_string(char *str)
{
	free(str);
}
