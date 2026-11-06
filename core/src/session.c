/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026, D-Modem
 *
 * session.c - the AT conversation with dmodemd (dmodemctl.h)
 *
 * The modem's bytes are read a line at a time (the ends of lines CR LF,
 * the CRs dropped).  A command of dmc_command()'s is written when the
 * one before it has its final result; the lines between its echo and
 * that result are its response.  Result codes nobody waits for (RING,
 * NO CARRIER, a CONNECT after an automatic answer) and the command
 * lines the user types in the terminal move the call's state too.
 *
 * Online, the bytes are the call's data and only NO CARRIER is looked
 * for; while a configurator runs, only the OK that ends it (or the
 * ERROR that refuses it).  Results need the modem's verbose result
 * codes (V1, its default).
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "dmodemctl.h"
#include "util.h"

/* the usual time a command may take, and dialling's or answering's */
#define COMMAND_MS 5000
#define CALL_MS 120000
/* the escape's guard time (S12's default, and a margin) */
#define GUARD_MS 1100
/* how long after the escape its OK may take */
#define ESCAPE_OK_MS 3000
/* RINGs are this far apart at most (a ring cadence is 6 s) */
#define RING_GAP_MS 8000
/* a line longer than this is cut (a configurator's screen has none) */
#define LINE_MAX 4096

struct command {
	int tag;                /* 0: the session's own */
	char *cmd;              /* with its AT */
	int flags, timeout_ms;
	struct command *next;
};

struct event {
	dmc_event ev;
	char *text, *result;
	struct event *next;
};

/* the steps of a hang-up from online */
enum { HANG_NONE, HANG_GUARD, HANG_ESCAPED };

struct dmc_session {
	int state;
	int64_t now;
	dmc_buf out, term, line, typed;
	/* the queue, and the one written that waits for its result */
	struct command *queue, *active;
	int64_t active_until;
	int echo_seen;
	dmc_buf response;
	int next_tag;
	/* the events, and the one dmc_next_event() last gave */
	struct event *events, *events_tail, *current;
	/* a hang-up from online: its step and when the next is due */
	int hang;
	int64_t hang_at;
	/* the user typed +++ online */
	int user_escape;
	int64_t ring_until;
	char *connect_text, *caller_number, *caller_name;
	int connect_rate;
};

static void command_free(struct command *c)
{
	if (!c)
		return;
	free(c->cmd);
	free(c);
}

static void event_free(struct event *e)
{
	if (!e)
		return;
	free(e->text);
	free(e->result);
	free(e);
}

static void push_event(dmc_session *s, int type, int tag, int ok,
		       const char *text, const char *result)
{
	struct event *e = calloc(1, sizeof(*e));
	if (!e)
		abort();
	e->text = dup_str(text);
	e->result = dup_str(result);
	e->ev.type = type;
	e->ev.tag = tag;
	e->ev.ok = ok;
	e->ev.state = s->state;
	e->ev.text = e->text;
	e->ev.result = e->result;
	if (s->events_tail)
		s->events_tail->next = e;
	else
		s->events = e;
	s->events_tail = e;
}

static void set_state(dmc_session *s, int state)
{
	if (s->state == state)
		return;
	s->state = state;
	if (state != DMC_RINGING) {
		s->ring_until = 0;
	}
	if (state == DMC_IDLE || state == DMC_CLOSED) {
		s->hang = HANG_NONE;
		s->user_escape = 0;
	}
	push_event(s, DMC_EV_STATE, 0, 1, "", "");
}

static void clear_queue(dmc_session *s)
{
	while (s->queue) {
		struct command *c = s->queue;
		s->queue = c->next;
		command_free(c);
	}
	command_free(s->active);
	s->active = NULL;
	buf_clear(&s->response);
}

dmc_session *dmc_session_new(void)
{
	dmc_session *s = calloc(1, sizeof(*s));
	if (!s)
		abort();
	s->next_tag = 1;
	s->connect_text = dup_str("");
	s->caller_number = dup_str("");
	s->caller_name = dup_str("");
	return s;
}

void dmc_session_free(dmc_session *s)
{
	if (!s)
		return;
	clear_queue(s);
	while (s->events) {
		struct event *e = s->events;
		s->events = e->next;
		event_free(e);
	}
	event_free(s->current);
	buf_free(&s->out);
	buf_free(&s->term);
	buf_free(&s->line);
	buf_free(&s->typed);
	buf_free(&s->response);
	free(s->connect_text);
	free(s->caller_number);
	free(s->caller_name);
	free(s);
}

void dmc_open(dmc_session *s, int64_t now_ms)
{
	clear_queue(s);
	buf_clear(&s->out);
	buf_clear(&s->line);
	buf_clear(&s->typed);
	s->now = now_ms;
	s->hang = HANG_NONE;
	set_state(s, DMC_IDLE);
}

void dmc_close(dmc_session *s)
{
	struct command *a = s->active;
	s->active = NULL;
	if (a && a->tag)
		push_event(s, DMC_EV_RESPONSE, a->tag, 0, "", "closed");
	command_free(a);
	while (s->queue) {
		struct command *c = s->queue;
		s->queue = c->next;
		if (c->tag)
			push_event(s, DMC_EV_RESPONSE, c->tag, 0, "", "closed");
		command_free(c);
	}
	buf_clear(&s->out);
	buf_clear(&s->line);
	buf_clear(&s->typed);
	buf_clear(&s->response);
	set_state(s, DMC_CLOSED);
}

/* the command's letter after AT and what comes before it, skipping
   spaces: its first command (D, A, H, O, or '!' for AT!) */
static char first_command(const char *cmd)
{
	const char *p = cmd;
	while (*p == ' ')
		p++;
	if (!starts_ci(p, "AT"))
		return 0;
	p += 2;
	while (*p == ' ')
		p++;
	return (char)toupper((unsigned char)*p);
}

/* the command starts a configurator: AT!C, AT!CF, AT!CT */
static int is_configurator(const char *cmd)
{
	const char *p = cmd;
	while (*p == ' ')
		p++;
	if (!starts_ci(p, "AT"))
		return 0;
	p += 2;
	while (*p == ' ')
		p++;
	return p[0] == '!' && toupper((unsigned char)p[1]) == 'C';
}

/* the state a command written puts the call in */
static void command_sent(dmc_session *s, const char *cmd)
{
	char c = first_command(cmd);
	if (is_configurator(cmd)) {
		if (s->state == DMC_IDLE)
			set_state(s, DMC_CONFIGURATOR);
	} else if (c == 'D') {
		if (s->state == DMC_IDLE || s->state == DMC_ONLINE_COMMAND)
			set_state(s, DMC_DIALING);
	} else if (c == 'A') {
		set_state(s, DMC_ANSWERING);
	} else if (c == 'H' && s->state == DMC_ONLINE_COMMAND) {
		set_state(s, DMC_HANGING_UP);
	}
}

/* the next command written, if none waits */
static void pump(dmc_session *s)
{
	if (s->active || !s->queue || s->state == DMC_CLOSED ||
	    s->state == DMC_ONLINE || s->state == DMC_CONFIGURATOR ||
	    s->state == DMC_DIALING || s->state == DMC_ANSWERING)
		return;
	if (s->hang == HANG_GUARD || s->hang == HANG_ESCAPED)
		return;
	struct command *c = s->queue;
	s->queue = c->next;
	c->next = NULL;
	s->active = c;
	s->active_until = s->now + c->timeout_ms;
	s->echo_seen = 0;
	buf_clear(&s->response);
	buf_adds(&s->out, c->cmd);
	buf_addc(&s->out, '\r');
	command_sent(s, c->cmd);
}

static int enqueue(dmc_session *s, const char *cmd, int flags,
		   int timeout_ms, int tag, int front)
{
	struct command *c = calloc(1, sizeof(*c));
	if (!c)
		abort();
	const char *p = cmd;
	while (*p == ' ')
		p++;
	if (starts_ci(p, "AT")) {
		c->cmd = dup_str(p);
	} else {
		size_t n = strlen(p);
		c->cmd = malloc(n + 3);
		if (!c->cmd)
			abort();
		memcpy(c->cmd, "AT", 2);
		memcpy(c->cmd + 2, p, n + 1);
	}
	/* (no CR or LF in it: one command line) */
	for (char *q = c->cmd; *q; q++)
		if (*q == '\r' || *q == '\n')
			*q = ' ';
	char first = first_command(c->cmd);
	c->tag = tag;
	c->flags = flags;
	c->timeout_ms = timeout_ms > 0 ? timeout_ms :
		(first == 'D' || first == 'A') ? CALL_MS : COMMAND_MS;
	if (front) {
		c->next = s->queue;
		s->queue = c;
	} else {
		struct command **q = &s->queue;
		while (*q)
			q = &(*q)->next;
		*q = c;
	}
	pump(s);
	return tag;
}

int dmc_command(dmc_session *s, const char *cmd, int flags, int timeout_ms)
{
	if (!cmd || s->state == DMC_CLOSED || s->state == DMC_ONLINE ||
	    s->state == DMC_CONFIGURATOR)
		return -1;
	int tag = s->next_tag++;
	if (s->next_tag <= 0)
		s->next_tag = 1;
	return enqueue(s, cmd, flags, timeout_ms, tag, 0);
}

int dmc_dial(dmc_session *s, const char *number)
{
	dmc_buf b = { 0 };
	buf_adds(&b, "ATD");
	for (const char *p = number ? number : ""; *p; p++)
		if (*p != '\r' && *p != '\n' && *p != ';')
			buf_addc(&b, *p);
	int tag = dmc_command(s, buf_str(&b), 0, 0);
	buf_free(&b);
	return tag;
}

int dmc_answer(dmc_session *s)
{
	return dmc_command(s, "ATA", 0, 0);
}

void dmc_hangup(dmc_session *s)
{
	switch (s->state) {
	case DMC_ONLINE:
		if (s->hang == HANG_NONE) {
			s->hang = HANG_GUARD;
			s->hang_at = s->now + GUARD_MS;
			set_state(s, DMC_HANGING_UP);
		}
		break;
	case DMC_DIALING:
	case DMC_ANSWERING:
		/* any character stops it, with NO CARRIER */
		buf_addc(&s->out, '\r');
		break;
	case DMC_IDLE:
	case DMC_RINGING:
	case DMC_ONLINE_COMMAND:
		enqueue(s, "ATH", 0, 0, 0, 1);
		break;
	default:
		break;
	}
}

/* the line is a final result code: its kind (0 none) */
enum { R_NONE, R_OK, R_ERROR, R_CONNECT, R_NO_CARRIER, R_FAIL };

static int result_kind(const char *l)
{
	if (!strcmp(l, "OK"))
		return R_OK;
	if (!strcmp(l, "ERROR") || !strcmp(l, "+FCERROR"))
		return R_ERROR;
	if (!strncmp(l, "CONNECT", 7) && (!l[7] || l[7] == ' '))
		return R_CONNECT;
	if (!strcmp(l, "NO CARRIER"))
		return R_NO_CARRIER;
	if (!strcmp(l, "BUSY") || !strcmp(l, "NO DIALTONE") ||
	    !strcmp(l, "NO DIAL TONE") || !strcmp(l, "NO ANSWER") ||
	    !strcmp(l, "DELAYED") || !strcmp(l, "BLACKLISTED") ||
	    !strcmp(l, "NO LINE"))
		return R_FAIL;
	return R_NONE;
}

static void connected(dmc_session *s, const char *l)
{
	set_str(&s->connect_text, l);
	s->connect_rate = atoi(l + 7);
	s->hang = HANG_NONE;
	set_state(s, DMC_ONLINE);
}

/* the caller ID's lines after a RING: "NMBR = 5551234" */
static int caller_line(dmc_session *s, const char *l)
{
	static const char *const names[] = { "DATE", "TIME", "NMBR", "NAME",
					      "MESG", "DDN_NMBR", NULL };
	for (int i = 0; names[i]; i++) {
		size_t n = strlen(names[i]);
		if (strncmp(l, names[i], n))
			continue;
		const char *p = l + n;
		while (*p == ' ')
			p++;
		if (*p != '=')
			continue;
		p++;
		while (*p == ' ')
			p++;
		if (!strcmp(names[i], "NMBR"))
			set_str(&s->caller_number, p);
		else if (!strcmp(names[i], "NAME"))
			set_str(&s->caller_name, p);
		dmc_buf b = { 0 };
		buf_adds(&b, names[i]);
		buf_addc(&b, '=');
		buf_adds(&b, p);
		push_event(s, DMC_EV_CALLER, 0, 1, buf_str(&b), "");
		buf_free(&b);
		return 1;
	}
	return 0;
}

/* a result nobody's command waited for */
static void unsolicited(dmc_session *s, const char *l, int kind)
{
	if (!strcmp(l, "RING")) {
		if (s->state == DMC_IDLE || s->state == DMC_RINGING) {
			if (s->state == DMC_IDLE) {
				set_str(&s->caller_number, "");
				set_str(&s->caller_name, "");
			}
			set_state(s, DMC_RINGING);
			s->ring_until = s->now + RING_GAP_MS;
		}
		push_event(s, DMC_EV_RESULT, 0, 1, "", l);
		return;
	}
	if (caller_line(s, l))
		return;
	switch (kind) {
	case R_CONNECT:
		connected(s, l);
		break;
	case R_NO_CARRIER:
	case R_FAIL:
		set_state(s, DMC_IDLE);
		break;
	case R_OK:
		if (s->state == DMC_HANGING_UP)
			set_state(s, DMC_IDLE);
		else if (s->state == DMC_DIALING || s->state == DMC_ANSWERING)
			set_state(s, DMC_IDLE);     /* ATD...; and the like */
		return;
	case R_ERROR:
		if (s->state == DMC_DIALING || s->state == DMC_ANSWERING ||
		    s->state == DMC_CONFIGURATOR)
			set_state(s, DMC_IDLE);
		return;
	default:
		return;
	}
	push_event(s, DMC_EV_RESULT, 0, kind != R_FAIL && kind != R_NO_CARRIER,
		   "", l);
}

/* the active command's final result */
static void finish(dmc_session *s, const char *result, int kind)
{
	struct command *c = s->active;
	s->active = NULL;
	char first = first_command(c->cmd);
	switch (kind) {
	case R_CONNECT:
		connected(s, result);
		break;
	case R_NO_CARRIER:
	case R_FAIL:
		set_state(s, DMC_IDLE);
		break;
	case R_OK:
		if (s->state == DMC_HANGING_UP || first == 'H' ||
		    (first == 'Z' && s->state != DMC_CONFIGURATOR))
			set_state(s, DMC_IDLE);
		else if (s->state == DMC_DIALING || s->state == DMC_ANSWERING ||
			 s->state == DMC_CONFIGURATOR)
			set_state(s, DMC_IDLE);
		break;
	case R_ERROR:
		if (s->state != DMC_ONLINE_COMMAND && s->state != DMC_IDLE)
			set_state(s, DMC_IDLE);
		break;
	}
	if (c->tag)
		push_event(s, DMC_EV_RESPONSE, c->tag,
			   kind == R_OK || kind == R_CONNECT,
			   buf_str(&s->response), result);
	command_free(c);
	buf_clear(&s->response);
	pump(s);
}

/* a whole line from the modem (no CR or LF) */
static void line_in(dmc_session *s, const char *l)
{
	/* online: the call's data, but for its end */
	if (s->state == DMC_ONLINE ||
	    (s->state == DMC_HANGING_UP && s->hang == HANG_GUARD)) {
		if (!strcmp(l, "NO CARRIER")) {
			set_state(s, DMC_IDLE);
			push_event(s, DMC_EV_RESULT, 0, 0, "", l);
		} else if (!strcmp(l, "OK") && s->user_escape) {
			s->user_escape = 0;
			set_state(s, DMC_ONLINE_COMMAND);
		}
		return;
	}
	if (s->state == DMC_HANGING_UP && s->hang == HANG_ESCAPED) {
		if (!strcmp(l, "OK")) {
			s->hang = HANG_NONE;
			enqueue(s, "ATH", 0, 0, 0, 1);
		} else if (!strcmp(l, "NO CARRIER")) {
			set_state(s, DMC_IDLE);
			push_event(s, DMC_EV_RESULT, 0, 0, "", l);
		}
		return;
	}
	/* a configurator: only its end */
	if (s->state == DMC_CONFIGURATOR) {
		const char *p = l;
		/* (its last screen may come before the OK on that line) */
		size_t n = strlen(p);
		int ok = n >= 2 && !strcmp(p + n - 2, "OK");
		int err = !strcmp(p, "ERROR");
		if (!ok && !err && strcmp(p, "RING"))
			return;
		if (s->active && (ok || err)) {
			finish(s, ok ? "OK" : "ERROR", ok ? R_OK : R_ERROR);
			return;
		}
		if (!strcmp(p, "RING")) {
			/* a RING ends it, with no OK */
			set_state(s, DMC_IDLE);
			unsolicited(s, p, R_NONE);
			return;
		}
		set_state(s, DMC_IDLE);
		return;
	}
	int kind = result_kind(l);
	if (s->active) {
		if (!s->echo_seen && same_ci(l, s->active->cmd)) {
			s->echo_seen = 1;
			return;
		}
		if (!*l && !s->response.n)
			return;
		if (!strcmp(l, "RING")) {
			/* a RING in a quiet response still reaches the
			   screen */
			if (s->active->flags & DMC_QUIET)
				buf_adds(&s->term, "\r\nRING\r\n");
			unsolicited(s, l, R_NONE);
			return;
		}
		if (kind != R_NONE) {
			/* (the response without its blank lines at the end) */
			while (s->response.n &&
			       s->response.p[s->response.n - 1] == '\n')
				buf_pop(&s->response);
			finish(s, l, kind);
			return;
		}
		if (s->response.n)
			buf_addc(&s->response, '\n');
		buf_adds(&s->response, l);
		return;
	}
	if (!*l)
		return;
	unsolicited(s, l, kind);
}

void dmc_feed(dmc_session *s, const char *data, int n)
{
	if (s->state == DMC_CLOSED)
		return;
	for (int i = 0; i < n; i++) {
		char c = data[i];
		int quiet = s->active && (s->active->flags & DMC_QUIET);
		if (!quiet)
			buf_addc(&s->term, c);
		if (c == '\n') {
			line_in(s, buf_str(&s->line));
			buf_clear(&s->line);
			continue;
		}
		if (c == '\r')
			continue;
		if (s->line.n < LINE_MAX)
			buf_addc(&s->line, c);
	}
}

void dmc_type(dmc_session *s, const char *data, int n)
{
	if (s->state == DMC_CLOSED)
		return;
	buf_add(&s->out, data, n);
	if (s->state == DMC_ONLINE) {
		/* the escape typed by hand: its OK is command mode */
		for (int i = 0; i < n; i++)
			if (data[i] == '+')
				s->user_escape = 1;
		return;
	}
	if (s->state == DMC_CONFIGURATOR)
		return;
	for (int i = 0; i < n; i++) {
		char c = data[i];
		if (c == '\r' || c == '\n') {
			if (s->typed.n)
				command_sent(s, buf_str(&s->typed));
			buf_clear(&s->typed);
		} else if (c == '\b' || c == 0x7f) {
			buf_pop(&s->typed);
		} else if (s->typed.n < LINE_MAX) {
			buf_addc(&s->typed, c);
		}
	}
}

int dmc_poll(dmc_session *s, int64_t now_ms)
{
	s->now = now_ms;
	int64_t next = -1;
	if (s->state == DMC_CLOSED)
		return -1;
	if (s->hang == HANG_GUARD && now_ms >= s->hang_at) {
		buf_adds(&s->out, "+++");
		s->hang = HANG_ESCAPED;
		s->hang_at = now_ms + GUARD_MS + ESCAPE_OK_MS;
	} else if (s->hang == HANG_ESCAPED && now_ms >= s->hang_at) {
		/* no OK: ATH all the same */
		s->hang = HANG_NONE;
		enqueue(s, "ATH", 0, 0, 0, 1);
	}
	if (s->hang != HANG_NONE)
		next = s->hang_at - now_ms;
	if (s->state == DMC_RINGING && s->ring_until && now_ms >= s->ring_until)
		set_state(s, DMC_IDLE);
	if (s->state == DMC_RINGING && s->ring_until) {
		int64_t d = s->ring_until - now_ms;
		if (next < 0 || d < next)
			next = d;
	}
	if (s->active) {
		if (now_ms >= s->active_until) {
			struct command *c = s->active;
			s->active = NULL;
			if (s->state == DMC_DIALING || s->state == DMC_ANSWERING) {
				buf_addc(&s->out, '\r');
				set_state(s, DMC_IDLE);
			}
			if (c->tag)
				push_event(s, DMC_EV_RESPONSE, c->tag, 0,
					   buf_str(&s->response), "timeout");
			command_free(c);
			buf_clear(&s->response);
		} else {
			int64_t d = s->active_until - now_ms;
			if (next < 0 || d < next)
				next = d;
		}
	}
	pump(s);
	if (s->active && next < 0)
		next = s->active_until - now_ms;
	if (next > 0x7fffffff)
		next = 0x7fffffff;
	return (int)next;
}

int dmc_output(dmc_session *s, char *buf, int max)
{
	return buf_take(&s->out, buf, max);
}

int dmc_terminal(dmc_session *s, char *buf, int max)
{
	return buf_take(&s->term, buf, max);
}

int dmc_next_event(dmc_session *s, dmc_event *ev)
{
	event_free(s->current);
	s->current = NULL;
	struct event *e = s->events;
	if (!e) {
		memset(ev, 0, sizeof(*ev));
		ev->text = ev->result = "";
		return 0;
	}
	s->events = e->next;
	if (!s->events)
		s->events_tail = NULL;
	s->current = e;
	*ev = e->ev;
	return 1;
}

int dmc_state(const dmc_session *s)
{
	return s->state;
}

const char *dmc_state_name(int state)
{
	switch (state) {
	case DMC_CLOSED: return "Not connected";
	case DMC_IDLE: return "Idle";
	case DMC_DIALING: return "Dialling";
	case DMC_RINGING: return "Ringing";
	case DMC_ANSWERING: return "Answering";
	case DMC_ONLINE: return "Online";
	case DMC_ONLINE_COMMAND: return "Online (command mode)";
	case DMC_HANGING_UP: return "Hanging up";
	case DMC_CONFIGURATOR: return "Configurator";
	}
	return "";
}

const char *dmc_connect_text(const dmc_session *s)
{
	return s->connect_text;
}

int dmc_connect_rate(const dmc_session *s)
{
	return s->connect_rate;
}

const char *dmc_caller_number(const dmc_session *s)
{
	return s->caller_number;
}

const char *dmc_caller_name(const dmc_session *s)
{
	return s->caller_name;
}

int dmc_version(void)
{
	return DMC_VERSION;
}
