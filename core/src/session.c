/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
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
 *
 * A voice call is AT+FCLASS=8 and AT+VSM=4 (G.711 mu-law), then ATD
 * (its OK the far end's answer) or AT+VLS=1 (the ringing call
 * answered), then AT+VTR: after its CONNECT the bytes both ways are
 * samples, <DLE> doubled, and the modem's <DLE> codes are what it heard
 * on the line.  <DLE>^ from us ends the stream (the modem's <DLE><ETX>
 * and OK), and ATH puts the modem on hook, in data mode again.
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "dmodemctl.h"
#include "util.h"
#include "voice.h"

/* the usual time a command may take, and dialling's or answering's */
#define COMMAND_MS 5000
#define CALL_MS 120000
/* the escape's guard time (S12's default, and a margin) */
#define GUARD_MS 1100
/* how long after the escape its OK may take */
#define ESCAPE_OK_MS 3000
/* RINGs are this far apart at most (a ring cadence is 6 s) */
#define RING_GAP_MS 8000
/* a line longer than this keeps only its end (a configurator's
   screens come with no line ends, and its OK after the last one) */
#define LINE_MAX 4096

/* voice: how far ahead of the line our samples go (60 ms), how many of
   the far end's are kept (1 s), how many of ours may wait (a third of
   a second: then they are cut to 100 ms) */
#define VOICE_LEAD (DMC_VOICE_RATE * 60 / 1000)
#define VOICE_RX_MAX DMC_VOICE_RATE
#define VOICE_TX_MAX (DMC_VOICE_RATE / 3)
#define VOICE_TX_KEEP (DMC_VOICE_RATE / 10)
/* how long the modem may take to end the stream (<DLE><ETX>, OK) */
#define VOICE_END_MS 3000
/* a DTMF digit's tone and the gap after it, in samples, and its
   level; the answering machine's beep */
#define DTMF_ON (DMC_VOICE_RATE / 10)
#define DTMF_OFF (DMC_VOICE_RATE * 6 / 100)
#define DTMF_AMP 16000
#define BEEP_HZ 1000
#define BEEP_LEN (DMC_VOICE_RATE / 2)
#define BEEP_AMP 8000
/* the answering machine: quieter than this is silence; its usual
   longest message and silence (s); the shortest message kept (1 s);
   how much of a busy or dial tone to cut from a message's end (2 s);
   the silence kept at its end (0.5 s) */
#define QUIET_LEVEL 400
#define MESSAGE_MAX_S 120
#define MESSAGE_SILENCE_S 7
#define MESSAGE_MIN (DMC_VOICE_RATE)
#define TONE_TRIM (2 * DMC_VOICE_RATE)
#define QUIET_KEEP (DMC_VOICE_RATE / 2)

#define DLE 0x10
#define ETX 0x03

/* the session's own flags on its commands (above dmc_command()'s) */
enum {
	CMD_VSETUP = 0x100,     /* AT+FCLASS=8, AT+VSM: voice mode */
	CMD_VCALL = 0x200,      /* ATD or AT+VLS=1: the call */
	CMD_VTR = 0x400,        /* AT+VTR: the stream */
	CMD_VHANG = 0x800       /* ATH: the call over, data mode */
};

/* what a voice call is for, and where it is */
enum { V_NONE, V_LIVE, V_MACHINE };
enum {
	VS_NONE,
	VS_SETUP,       /* the commands before the stream */
	VS_STREAM,      /* samples both ways */
	VS_ENDING,      /* <DLE>^ sent, the modem's <DLE><ETX> awaited */
	VS_ENDED,       /* <DLE><ETX> come, its OK awaited */
	VS_HANGUP       /* ATH */
};
/* the answering machine's steps */
enum { MP_GREETING, MP_BEEP, MP_RECORD };

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
	int rings;
	/* a voice call */
	int voice, vstep;
	int vdle, vpacket;      /* a <DLE> came; inside a <DLE>X packet */
	int vhang;              /* to be ended once its stream starts */
	int64_t vstart, vsent, vend_at;
	dmc_ring rx, tx;
	char dtmf[32];
	int dtmf_n, tone_gap;
	dmc_tone tone, beep;
	/* the answering machine */
	int16_t *greeting;
	int greeting_n, max_s, silence_s, auto_rings;
	int mphase, gpos, mdone;
	int16_t *rec, *msg;
	int rec_n, rec_cap, quiet, msg_n;
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

static void machine_done(dmc_session *s, int trim);
static void voice_reset(dmc_session *s);

static void set_state(dmc_session *s, int state)
{
	if (s->state == state)
		return;
	if (state == DMC_IDLE || state == DMC_CLOSED) {
		if (s->voice == V_MACHINE && !s->mdone)
			machine_done(s, 0);
		voice_reset(s);
	}
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

/* the voice call's state: the far end's samples, ours, the tones */
static void voice_reset(dmc_session *s)
{
	s->voice = V_NONE;
	s->vstep = VS_NONE;
	s->vdle = s->vpacket = s->vhang = 0;
	ring_clear(&s->rx);
	ring_clear(&s->tx);
	s->dtmf_n = s->tone_gap = 0;
	s->tone.left = s->beep.left = 0;
	free(s->rec);
	s->rec = NULL;
	s->rec_n = s->rec_cap = s->quiet = 0;
	s->mphase = MP_GREETING;
	s->gpos = s->mdone = 0;
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
	ring_init(&s->rx, VOICE_RX_MAX);
	ring_init(&s->tx, DMC_VOICE_RATE);
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
	free(s->rec);
	free(s->msg);
	free(s->greeting);
	ring_free(&s->rx);
	ring_free(&s->tx);
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
	voice_reset(s);
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
	/* (the stream's bytes are samples, not commands) */
	if (s->vstep == VS_STREAM || s->vstep == VS_ENDING)
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
	/* (AT+VLS=1 answers the ringing call) */
	if ((c->flags & CMD_VCALL) && s->state != DMC_DIALING)
		set_state(s, DMC_ANSWERING);
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
	    s->state == DMC_CONFIGURATOR || s->state == DMC_VOICE ||
	    s->state == DMC_VOICEMAIL || s->voice)
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

static void voice_end(dmc_session *s);

void dmc_hangup(dmc_session *s)
{
	if (s->voice && s->state != DMC_DIALING) {
		if (s->voice == V_MACHINE && !s->mdone &&
		    s->mphase == MP_RECORD)
			machine_done(s, 0);
		voice_end(s);
		return;
	}
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
				s->rings = 0;
			}
			set_state(s, DMC_RINGING);
			s->ring_until = s->now + RING_GAP_MS;
			s->rings++;
		}
		push_event(s, DMC_EV_RESULT, 0, 1, "", l);
		/* the answering machine's turn */
		if (s->state == DMC_RINGING && s->auto_rings > 0 &&
		    s->rings >= s->auto_rings && !s->voice && !s->active &&
		    !s->queue)
			dmc_voicemail_answer(s);
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

static void voice_fail(dmc_session *s, const char *result);
static void stream_start(dmc_session *s);

/* a voice command's final result: 1 if it was one */
static int voice_finish(dmc_session *s, struct command *c, const char *result,
			int kind)
{
	int ok;
	if (!(c->flags & (CMD_VSETUP | CMD_VCALL | CMD_VTR | CMD_VHANG)))
		return 0;
	ok = kind == R_OK || ((c->flags & CMD_VTR) && kind == R_CONNECT);
	if (c->tag)
		push_event(s, DMC_EV_RESPONSE, c->tag, ok,
			   buf_str(&s->response), result);
	buf_clear(&s->response);
	if (c->flags & CMD_VHANG) {
		command_free(c);
		set_state(s, DMC_IDLE);
	} else if (!s->voice) {
		/* (a call given up while its command ran) */
		command_free(c);
	} else if (!ok) {
		command_free(c);
		voice_fail(s, result);
	} else if (c->flags & CMD_VCALL) {
		command_free(c);
		set_state(s, s->voice == V_MACHINE ? DMC_VOICEMAIL : DMC_VOICE);
		enqueue(s, "AT+VTR", DMC_QUIET | CMD_VTR, COMMAND_MS, 0, 1);
		return 1;
	} else if (c->flags & CMD_VTR) {
		command_free(c);
		stream_start(s);
	} else {
		command_free(c);
	}
	pump(s);
	return 1;
}

/* the active command's final result */
static void finish(dmc_session *s, const char *result, int kind)
{
	struct command *c = s->active;
	s->active = NULL;
	if (voice_finish(s, c, result, kind))
		return;
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
	/* the stream over: its OK, then ATH */
	if (s->vstep == VS_ENDED) {
		if (!strcmp(l, "OK") || !strcmp(l, "ERROR")) {
			s->vstep = VS_HANGUP;
			enqueue(s, "ATH", DMC_QUIET | CMD_VHANG, 0, 0, 1);
		}
		return;
	}
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

static void voice_byte(dmc_session *s, unsigned char c);

void dmc_feed(dmc_session *s, const char *data, int n)
{
	if (s->state == DMC_CLOSED)
		return;
	for (int i = 0; i < n; i++) {
		char c = data[i];
		if (s->vstep == VS_STREAM || s->vstep == VS_ENDING) {
			voice_byte(s, (unsigned char)c);
			continue;
		}
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
		if (s->line.n >= LINE_MAX)
			buf_drop(&s->line, s->line.n - LINE_MAX / 2);
		buf_addc(&s->line, c);
	}
}

void dmc_type(dmc_session *s, const char *data, int n)
{
	if (s->state == DMC_CLOSED)
		return;
	/* (nothing typed gets into a voice call's samples) */
	if (s->voice)
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

static void voice_send(dmc_session *s);

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
	if (s->vstep == VS_STREAM) {
		voice_send(s);
		next = 20;
	} else if ((s->vstep == VS_ENDING || s->vstep == VS_ENDED) &&
		   now_ms >= s->vend_at) {
		/* no end from the modem: on hook all the same */
		if (s->vstep == VS_ENDING) {
			buf_addc(&s->out, DLE);
			buf_addc(&s->out, '^');
		}
		s->vstep = VS_HANGUP;
		enqueue(s, "ATH", DMC_QUIET | CMD_VHANG, 0, 0, 1);
	} else if (s->vstep == VS_ENDING || s->vstep == VS_ENDED) {
		int64_t d = s->vend_at - now_ms;
		if (next < 0 || d < next)
			next = d;
	}
	if (s->state == DMC_RINGING && s->ring_until && now_ms >= s->ring_until)
		set_state(s, DMC_IDLE);
	/* (the answering machine's turn, if a command was in the way at
	   the RING) */
	if (s->state == DMC_RINGING && s->auto_rings > 0 &&
	    s->rings >= s->auto_rings && !s->voice && !s->active &&
	    !s->queue)
		dmc_voicemail_answer(s);
	if (s->state == DMC_RINGING && s->ring_until) {
		int64_t d = s->ring_until - now_ms;
		if (next < 0 || d < next)
			next = d;
	}
	if (s->active) {
		if (now_ms >= s->active_until) {
			struct command *c = s->active;
			int voice = c->flags & (CMD_VSETUP | CMD_VCALL |
						CMD_VTR | CMD_VHANG);
			s->active = NULL;
			if (s->state == DMC_DIALING || s->state == DMC_ANSWERING) {
				buf_addc(&s->out, '\r');
				if (!voice)
					set_state(s, DMC_IDLE);
			}
			if (c->tag)
				push_event(s, DMC_EV_RESPONSE, c->tag, 0,
					   buf_str(&s->response), "timeout");
			command_free(c);
			buf_clear(&s->response);
			if (voice & CMD_VHANG)
				set_state(s, DMC_IDLE);
			else if (voice && s->voice)
				voice_fail(s, "timeout");
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
	case DMC_VOICE: return "Voice call";
	case DMC_VOICEMAIL: return "Taking a message";
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

/*
 * Voice calls
 */

/* the voice call given up (a command of it failed): its commands still
   queued answered with result, the modem on hook in data mode */
static void voice_fail(dmc_session *s, const char *result)
{
	struct command **q = &s->queue;
	while (*q) {
		struct command *c = *q;
		if (!(c->flags & (CMD_VSETUP | CMD_VCALL | CMD_VTR))) {
			q = &c->next;
			continue;
		}
		*q = c->next;
		if (c->tag)
			push_event(s, DMC_EV_RESPONSE, c->tag, 0, "", result);
		command_free(c);
	}
	if (s->state == DMC_IDLE) {
		if (s->voice == V_MACHINE && !s->mdone)
			machine_done(s, 0);
		voice_reset(s);
	} else {
		set_state(s, DMC_IDLE);
	}
	enqueue(s, "ATH", DMC_QUIET | CMD_VHANG, 0, 0, 1);
}

/* AT+VTR's CONNECT: the samples flow */
static void stream_start(dmc_session *s)
{
	s->vstep = VS_STREAM;
	s->vstart = s->now;
	s->vsent = 0;
	s->vdle = s->vpacket = 0;
	ring_clear(&s->rx);
	ring_clear(&s->tx);
	s->mphase = MP_GREETING;
	s->gpos = 0;
	if (s->vhang)
		voice_end(s);
}

/* the call ended from here: the stream (<DLE>^), or a call still being
   set up once its stream starts */
static void voice_end(dmc_session *s)
{
	switch (s->vstep) {
	case VS_SETUP:
		s->vhang = 1;
		break;
	case VS_STREAM:
		buf_addc(&s->out, DLE);
		buf_addc(&s->out, '^');
		s->vstep = VS_ENDING;
		s->vend_at = s->now + VOICE_END_MS;
		set_state(s, DMC_HANGING_UP);
		break;
	default:
		break;
	}
}

/* the answering machine's message over: trim samples of its end cut
   (a busy tone's), then its quiet end but for QUIET_KEEP; the
   DMC_EV_MESSAGE */
static void machine_done(dmc_session *s, int trim)
{
	int n = s->rec_n - trim, ok;
	if (s->mdone)
		return;
	s->mdone = 1;
	if (s->mphase == MP_RECORD && s->quiet > QUIET_KEEP) {
		int q = s->quiet - QUIET_KEEP;
		if (trim < q)
			n = s->rec_n - q;
	}
	ok = s->mphase == MP_RECORD && n >= MESSAGE_MIN;
	free(s->msg);
	s->msg = NULL;
	s->msg_n = 0;
	if (ok) {
		s->msg = s->rec;
		s->msg_n = n;
		s->rec = NULL;
		s->rec_n = s->rec_cap = 0;
	}
	push_event(s, DMC_EV_MESSAGE, 0, ok, s->caller_number, "");
}

/* a sample of the caller's for the message: the message over when it is
   long enough, or the caller silent long enough */
static void record(dmc_session *s, int16_t x)
{
	int max = (s->max_s > 0 ? s->max_s : MESSAGE_MAX_S) * DMC_VOICE_RATE;
	int quiet = (s->silence_s > 0 ? s->silence_s : MESSAGE_SILENCE_S) *
		    DMC_VOICE_RATE;
	if (s->mdone)
		return;
	if (s->rec_n == s->rec_cap) {
		int cap = s->rec_cap ? s->rec_cap * 2 : DMC_VOICE_RATE * 8;
		int16_t *p;
		if (cap > max)
			cap = max;
		p = realloc(s->rec, (size_t)cap * sizeof(int16_t));
		if (!p)
			abort();
		s->rec = p;
		s->rec_cap = cap;
	}
	s->rec[s->rec_n++] = x;
	if (x > -QUIET_LEVEL && x < QUIET_LEVEL)
		s->quiet++;
	else
		s->quiet = 0;
	if (s->rec_n >= max || s->quiet >= quiet) {
		machine_done(s, 0);
		voice_end(s);
	}
}

/* a sample of the far end's */
static void voice_sample(dmc_session *s, unsigned char u)
{
	int16_t x = ulaw_decode(u);
	ring_put(&s->rx, &x, 1);
	if (s->voice == V_MACHINE && s->mphase == MP_RECORD)
		record(s, x);
}

/* a <DLE> code from the modem: what it heard on the line */
static void voice_event(dmc_session *s, unsigned char c)
{
	const char *name;
	char code[2];
	if ((c >= '0' && c <= '9') || c == '*' || c == '#' ||
	    (c >= 'A' && c <= 'D'))
		name = "digit";
	else switch (c) {
	case 'l': name = "hangup"; break;
	case 'b': name = "busy"; break;
	case 'd': name = "dial tone"; break;
	case 'r': name = "ringing"; break;
	case 'a': case 'f': name = "answer tone"; break;
	case 'c': name = "fax"; break;
	case 'e': name = "data"; break;
	case 's': name = "silence"; break;
	case 'q': name = "quiet"; break;
	default: return;
	}
	code[0] = (char)c;
	code[1] = '\0';
	push_event(s, DMC_EV_VOICE, 0, 1, code, name);
	if (c == 'l' || c == 'b' || c == 'd') {
		/* the far end has gone (a busy or dial tone: the
		   exchange's word for it) */
		if (s->voice == V_MACHINE && s->mphase == MP_RECORD)
			machine_done(s, c == 'l' ? 0 : TONE_TRIM);
		voice_end(s);
	}
}

/* a byte of the stream from the modem */
static void voice_byte(dmc_session *s, unsigned char c)
{
	if (s->vdle) {
		s->vdle = 0;
		switch (c) {
		case DLE:
			if (!s->vpacket)
				voice_sample(s, c);
			break;
		case ETX:
			/* the stream's end: its OK to come */
			s->vstep = VS_ENDED;
			s->vend_at = s->now + VOICE_END_MS;
			if (s->state != DMC_HANGING_UP)
				set_state(s, DMC_HANGING_UP);
			buf_clear(&s->line);
			break;
		case 'X':
			s->vpacket = 1;
			break;
		case '.':
			s->vpacket = 0;
			break;
		default:
			voice_event(s, c);
			break;
		}
		return;
	}
	if (c == DLE) {
		s->vdle = 1;
		return;
	}
	if (!s->vpacket)
		voice_sample(s, c);
}

/* our next sample: the answering machine's greeting and beep, or the
   microphone's; the DTMF digits over it */
static int next_sample(dmc_session *s)
{
	int x = 0;
	int16_t m;
	if (s->voice == V_MACHINE) {
		if (s->mphase == MP_GREETING) {
			if (s->gpos < s->greeting_n) {
				x = s->greeting[s->gpos++];
			} else {
				s->mphase = MP_BEEP;
				tone_start(&s->beep, BEEP_HZ, 0, BEEP_LEN,
					   BEEP_AMP);
			}
		}
		if (s->mphase == MP_BEEP) {
			if (s->beep.left > 0) {
				x = tone_next(&s->beep);
			} else {
				s->mphase = MP_RECORD;
				s->rec_n = 0;
				s->quiet = 0;
			}
		}
	} else if (ring_get(&s->tx, &m, 1)) {
		x = m;
	}
	if (s->tone.left > 0) {
		x += tone_next(&s->tone);
	} else if (s->tone_gap > 0) {
		s->tone_gap--;
	} else if (s->dtmf_n) {
		double lo, hi;
		if (!dtmf_freqs(s->dtmf[0], &lo, &hi))
			tone_start(&s->tone, lo, hi, DTMF_ON, DTMF_AMP);
		s->tone_gap = DTMF_OFF;
		memmove(s->dtmf, s->dtmf + 1, (size_t)--s->dtmf_n);
	}
	return x > 32767 ? 32767 : x < -32768 ? -32768 : x;
}

/* our samples to the modem, as many as the time says, a little
   ahead */
static void voice_send(dmc_session *s)
{
	int64_t due = (s->now - s->vstart) * DMC_VOICE_RATE / 1000 +
		      VOICE_LEAD;
	/* (a second behind: the app stalled, and the line went on) */
	if (due - s->vsent > DMC_VOICE_RATE)
		s->vsent = due - VOICE_LEAD;
	while (s->vsent < due && s->vstep == VS_STREAM) {
		unsigned char u = ulaw_encode((int16_t)next_sample(s));
		buf_addc(&s->out, (char)u);
		if (u == DLE)
			buf_addc(&s->out, (char)DLE);
		s->vsent++;
	}
}

/* the voice call's start: voice mode, mu-law, then the call's command
   (with the tag) */
static int voice_begin(dmc_session *s, int voice, const char *cmd,
		       int timeout_ms)
{
	int tag = s->next_tag++;
	if (s->next_tag <= 0)
		s->next_tag = 1;
	s->voice = voice;
	s->vstep = VS_SETUP;
	s->vhang = 0;
	s->mdone = 0;
	enqueue(s, "AT+FCLASS=8", DMC_QUIET | CMD_VSETUP, 0, 0, 0);
	enqueue(s, "AT+VSM=4", DMC_QUIET | CMD_VSETUP, 0, 0, 0);
	return enqueue(s, cmd, CMD_VCALL, timeout_ms, tag, 0);
}

int dmc_voice_dial(dmc_session *s, const char *number)
{
	dmc_buf b = { 0 };
	int tag;
	if (s->state != DMC_IDLE || s->voice)
		return -1;
	buf_adds(&b, "ATD");
	for (const char *p = number ? number : ""; *p; p++)
		if (*p != '\r' && *p != '\n' && *p != ';')
			buf_addc(&b, *p);
	tag = voice_begin(s, V_LIVE, buf_str(&b), CALL_MS);
	buf_free(&b);
	return tag;
}

static int answer_as(dmc_session *s, int voice)
{
	if (s->state != DMC_RINGING || s->voice)
		return -1;
	return voice_begin(s, voice, "AT+VLS=1", 30000);
}

int dmc_voice_answer(dmc_session *s)
{
	return answer_as(s, V_LIVE);
}

int dmc_voicemail_answer(dmc_session *s)
{
	return answer_as(s, V_MACHINE);
}

int dmc_voice_read(dmc_session *s, int16_t *pcm, int max)
{
	return max > 0 ? ring_get(&s->rx, pcm, max) : 0;
}

void dmc_voice_write(dmc_session *s, const int16_t *pcm, int n)
{
	if (s->vstep != VS_STREAM || n <= 0)
		return;
	ring_put(&s->tx, pcm, n);
	if (s->tx.n > VOICE_TX_MAX)
		ring_keep(&s->tx, VOICE_TX_KEEP);
}

int dmc_voice_dtmf(dmc_session *s, char digit)
{
	double lo, hi;
	if (!s->voice || s->dtmf_n >= (int)sizeof(s->dtmf) ||
	    dtmf_freqs(digit, &lo, &hi))
		return -1;
	s->dtmf[s->dtmf_n++] = digit;
	return 0;
}

void dmc_voicemail_set(dmc_session *s, const int16_t *greeting, int n,
		       int max_s, int silence_s)
{
	free(s->greeting);
	s->greeting = NULL;
	s->greeting_n = 0;
	if (greeting && n > 0) {
		s->greeting = malloc((size_t)n * sizeof(int16_t));
		if (!s->greeting)
			abort();
		memcpy(s->greeting, greeting, (size_t)n * sizeof(int16_t));
		s->greeting_n = n;
	}
	s->max_s = max_s;
	s->silence_s = silence_s;
}

void dmc_voicemail_auto(dmc_session *s, int rings)
{
	s->auto_rings = rings > 0 ? rings : 0;
}

int dmc_voice_pickup(dmc_session *s)
{
	if (s->voice != V_MACHINE || s->mdone)
		return -1;
	s->voice = V_LIVE;
	s->mdone = 1;
	s->beep.left = 0;
	free(s->rec);
	s->rec = NULL;
	s->rec_n = s->rec_cap = 0;
	if (s->state == DMC_VOICEMAIL)
		set_state(s, DMC_VOICE);
	return 0;
}

int dmc_voice_message(const dmc_session *s, const int16_t **pcm)
{
	*pcm = s->msg;
	return s->msg_n;
}

int dmc_ring_count(const dmc_session *s)
{
	return s->state == DMC_RINGING ? s->rings : 0;
}
