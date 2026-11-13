/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * path.c - the connection path: the picture at the top of the Settings
 *          page of how a call is made, step by step (the D-Modem app's
 *          ConnectionPath, for the desktop's lines)
 *
 *   dmodemd's own line:  Terminal -> D-Modem -> SIP, USB modem or codec
 *                        -> the phone network, SIP addresses, a phone line
 *   a modem that runs:   Terminal -> its device, TCP or COM port -> the
 *                        modem -> its phone line
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dmodemctl.h"

#define STEPS 4

typedef struct Step {
	int kind, state;
	const char *role;
	char title[32], detail[400];
} Step;

struct dmc_path {
	/* the settings */
	int line, anonymous, listen;
	char where[256], user[128], server[256];
	/* now */
	int running, state;
	char connect[128];
	/* what they make */
	Step step[STEPS];
	char headline[512];
};

static void copy(char *d, size_t n, const char *s)
{
	snprintf(d, n, "%s", s ? s : "");
}

static void step(Step *s, int kind, const char *title, int state,
		 const char *detail)
{
	s->kind = kind;
	s->state = state;
	copy(s->title, sizeof(s->title), title);
	copy(s->detail, sizeof(s->detail), detail);
}

static int rank(int state)
{
	switch (state) {
	case DMC_STEP_OFF: return 0;
	case DMC_STEP_SETUP: return 1;
	case DMC_STEP_WORKING: return 2;
	case DMC_STEP_READY: return 3;
	default: return 4;
	}
}

/* the steps and the headline, from the settings and now */
static void make(dmc_path *p)
{
	static const char *const roles[] = { "Program", "Modem", "Line", "Reaches" };
	static const char *const external[] = { "Program", "Port", "Modem", "Line" };
	Step *s = p->step;
	char b[400];
	int own = p->line == DMC_LINE_SIP || p->line == DMC_LINE_USB ||
		p->line == DMC_LINE_CODEC;

	step(&s[0], DMC_KIND_TERMINAL, "Terminal", DMC_STEP_READY,
	     "AT commands and calls");
	if (own) {
		int st = p->running && p->state == DMC_CLOSED ? DMC_STEP_WORKING :
			DMC_STEP_READY;
		step(&s[1], DMC_KIND_SOFTMODEM, "D-Modem", st,
		     st == DMC_STEP_WORKING ? "Starting" :
		     p->running ? "Running" : "dmodemd, the V.92 softmodem");
	}
	switch (p->line) {
	case DMC_LINE_USB:
		snprintf(b, sizeof(b), "%s", *p->where && strcmp(p->where, "auto") ?
			 p->where : "The first one found");
		step(&s[2], DMC_KIND_USB, "USB modem", DMC_STEP_READY, b);
		step(&s[3], DMC_KIND_PHONE_LINE, "Phone line", DMC_STEP_READY,
		     "The USB modem's");
		break;
	case DMC_LINE_CODEC:
		step(&s[2], DMC_KIND_CODEC, "Codec", *p->where ? DMC_STEP_READY :
		     DMC_STEP_SETUP, *p->where ? p->where : "No ALSA device set");
		step(&s[3], DMC_KIND_PHONE_LINE, "Phone line", DMC_STEP_READY,
		     "The codec's");
		break;
	case DMC_LINE_DEVICE:
	case DMC_LINE_TCP:
	case DMC_LINE_COM: {
		int set = *p->where != '\0';
		int st = !set ? DMC_STEP_SETUP :
			p->running && p->state == DMC_CLOSED ? DMC_STEP_WORKING :
			DMC_STEP_READY;
		const char *title = p->line == DMC_LINE_TCP ? "TCP" :
			p->line == DMC_LINE_COM ? "COM port" : "Device";
		step(&s[1], p->line == DMC_LINE_TCP ? DMC_KIND_NETWORK :
		     DMC_KIND_SERIAL, title, st,
		     set ? p->where : "Not set yet");
		step(&s[2], p->line == DMC_LINE_TCP ? DMC_KIND_SOFTMODEM :
		     DMC_KIND_MODEM, p->line == DMC_LINE_TCP ? "D-Modem" : "Modem",
		     set ? DMC_STEP_READY : DMC_STEP_SETUP,
		     p->running && p->state != DMC_CLOSED ? "Running" :
		     "Already running");
		step(&s[3], DMC_KIND_PHONE_LINE, "Phone line", DMC_STEP_READY,
		     "The modem's own");
		break;
	}
	default:
		if (p->anonymous) {
			snprintf(b, sizeof(b), "Anonymous: nothing registered%s",
				 p->listen ? ", takes direct calls" : "");
			step(&s[2], DMC_KIND_SIP, "SIP", DMC_STEP_READY, b);
			step(&s[3], DMC_KIND_SIP_ADDRESS, "SIP addresses",
			     DMC_STEP_READY, "Dialled directly (sip:user@host)");
		} else if (!*p->user || !*p->server) {
			step(&s[2], DMC_KIND_SIP, "SIP", DMC_STEP_SETUP,
			     "No account set up");
			step(&s[3], DMC_KIND_PHONE_NETWORK, "Phone network",
			     DMC_STEP_SETUP, "Through a SIP provider");
		} else {
			snprintf(b, sizeof(b), "%s %s@%s",
				 p->listen ? "Registers as" : "Account",
				 p->user, p->server);
			step(&s[2], DMC_KIND_SIP, "SIP", DMC_STEP_READY, b);
			snprintf(b, sizeof(b), "Through %s", p->server);
			step(&s[3], DMC_KIND_PHONE_NETWORK, "Phone network",
			     DMC_STEP_READY, b);
		}
		break;
	}

	/* a call: each step in use lights; idle, as set up */
	int now = -1;
	switch (p->state) {
	case DMC_ONLINE:
	case DMC_ONLINE_COMMAND:
	case DMC_VOICE:
	case DMC_VOICEMAIL:
		now = DMC_STEP_ACTIVE;
		break;
	case DMC_DIALING:
	case DMC_ANSWERING:
	case DMC_RINGING:
	case DMC_HANGING_UP:
		now = DMC_STEP_WORKING;
		break;
	}
	int setup = 0;
	for (int i = 0; i < STEPS; i++) {
		s[i].role = own ? roles[i] : external[i];
		if (now >= 0 && s[i].state != DMC_STEP_OFF &&
		    s[i].state != DMC_STEP_SETUP)
			s[i].state = now;
		if (s[i].state == DMC_STEP_SETUP && !setup) {
			setup = 1;
			char d[400];
			copy(d, sizeof(d), s[i].detail);
			if (d[0] >= 'A' && d[0] <= 'Z')
				d[0] = (char)(d[0] - 'A' + 'a');
			snprintf(p->headline, sizeof(p->headline),
				 "Not ready: %s (%s)", s[i].title, d);
		}
	}
	if (now == DMC_STEP_ACTIVE && (p->state == DMC_VOICE ||
				       p->state == DMC_VOICEMAIL))
		copy(p->headline, sizeof(p->headline), "In a voice call");
	else if (now == DMC_STEP_ACTIVE)
		snprintf(p->headline, sizeof(p->headline), "Online%s%s",
			 *p->connect ? ": " : "", p->connect);
	else if (p->state == DMC_RINGING)
		copy(p->headline, sizeof(p->headline), "Ringing");
	else if (p->state == DMC_HANGING_UP)
		copy(p->headline, sizeof(p->headline), "Hanging up");
	else if (now == DMC_STEP_WORKING)
		copy(p->headline, sizeof(p->headline), "Connecting");
	else if (!setup && p->running && p->state == DMC_CLOSED)
		copy(p->headline, sizeof(p->headline), "Starting the modem");
	else if (!setup && p->running)
		copy(p->headline, sizeof(p->headline), "Ready: the modem is running");
	else if (!setup)
		copy(p->headline, sizeof(p->headline),
		     "Ready: start the modem to dial");
}

dmc_path *dmc_path_new(void)
{
	dmc_path *p = calloc(1, sizeof(*p));
	if (p)
		make(p);
	return p;
}

void dmc_path_free(dmc_path *p)
{
	free(p);
}

void dmc_path_set_line(dmc_path *p, int line, const char *where,
		       int sip_anonymous, const char *sip_user,
		       const char *sip_server, int listen)
{
	p->line = line;
	copy(p->where, sizeof(p->where), where);
	p->anonymous = sip_anonymous != 0;
	copy(p->user, sizeof(p->user), sip_user);
	copy(p->server, sizeof(p->server), sip_server);
	p->listen = listen != 0;
	make(p);
}

void dmc_path_set_live(dmc_path *p, int running, int state,
		       const char *connect)
{
	p->running = running != 0;
	p->state = state;
	copy(p->connect, sizeof(p->connect), connect);
	make(p);
}

int dmc_path_steps(const dmc_path *p)
{
	(void)p;
	return STEPS;
}

static const Step *at(const dmc_path *p, int i)
{
	static const Step none = { 0, DMC_STEP_OFF, "", "", "" };
	return i >= 0 && i < STEPS ? &p->step[i] : &none;
}

int dmc_path_kind(const dmc_path *p, int i) { return at(p, i)->kind; }
int dmc_path_state(const dmc_path *p, int i) { return at(p, i)->state; }
const char *dmc_path_role(const dmc_path *p, int i) { return at(p, i)->role; }
const char *dmc_path_title(const dmc_path *p, int i) { return at(p, i)->title; }
const char *dmc_path_detail(const dmc_path *p, int i) { return at(p, i)->detail; }

int dmc_path_link(const dmc_path *p, int i)
{
	int a = at(p, i)->state, b = at(p, i + 1)->state;
	return rank(a) <= rank(b) ? a : b;
}

const char *dmc_path_headline(const dmc_path *p)
{
	return p->headline;
}

int dmc_path_needs_setup(const dmc_path *p)
{
	for (int i = 0; i < STEPS; i++)
		if (p->step[i].state == DMC_STEP_SETUP)
			return 1;
	return 0;
}
