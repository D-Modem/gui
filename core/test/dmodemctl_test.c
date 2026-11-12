/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * dmodemctl_test.c - libdmodemctl's tests: a session against a scripted
 * modem, AT!M and AT#UD as dmodemd prints them, and the terminal screen
 *
 *     dmodemctl_test <the directory of model.txt>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dmodemctl.h"

static int failures, checks;

#define CHECK(c) check((c), #c, __FILE__, __LINE__)
#define CHECK_STR(a, b) check_str((a), (b), #a, __FILE__, __LINE__)

static void check(int ok, const char *what, const char *file, int line)
{
	checks++;
	if (!ok) {
		failures++;
		fprintf(stderr, "%s:%d: failed: %s\n", file, line, what);
	}
}

static void check_str(const char *a, const char *b, const char *what,
		      const char *file, int line)
{
	checks++;
	if (strcmp(a ? a : "(null)", b ? b : "(null)")) {
		failures++;
		fprintf(stderr, "%s:%d: %s is \"%s\", not \"%s\"\n", file, line,
			what, a ? a : "(null)", b);
	}
}

/* what the session wrote to the modem, as text */
static const char *out(dmc_session *s)
{
	static char b[4096];
	int n = dmc_output(s, b, sizeof(b) - 1);
	b[n] = '\0';
	return b;
}

static const char *term(dmc_session *s)
{
	static char b[8192];
	int n = dmc_terminal(s, b, sizeof(b) - 1);
	b[n] = '\0';
	return b;
}

static void feed(dmc_session *s, const char *t)
{
	dmc_feed(s, t, (int)strlen(t));
}

/* the next event of type, skipping the others; 0 if none */
static int next_of(dmc_session *s, int type, dmc_event *ev)
{
	while (dmc_next_event(s, ev))
		if (ev->type == type)
			return 1;
	return 0;
}

static char *read_file(const char *dir, const char *name)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "can't open %s\n", path);
		exit(2);
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *b = malloc((size_t)n + 1);
	if (!b || fread(b, 1, (size_t)n, f) != (size_t)n)
		exit(2);
	b[n] = '\0';
	fclose(f);
	return b;
}

/* a command, its echo, its response and OK */
static void test_command(void)
{
	dmc_session *s = dmc_session_new();
	dmc_event ev;
	CHECK(dmc_state(s) == DMC_CLOSED);
	CHECK(dmc_command(s, "ATI3", 0, 0) == -1);
	dmc_open(s, 0);
	CHECK(dmc_state(s) == DMC_IDLE);
	int tag = dmc_command(s, "I3", 0, 0);
	CHECK(tag > 0);
	CHECK_STR(out(s), "ATI3\r");
	/* a second waits for the first */
	int tag2 = dmc_command(s, "ATI0", DMC_QUIET, 0);
	CHECK_STR(out(s), "");
	feed(s, "ATI3\r\r\nD-Modem 1.0.0\r\n\r\nOK\r\n");
	CHECK(next_of(s, DMC_EV_RESPONSE, &ev));
	CHECK(ev.tag == tag);
	CHECK(ev.ok);
	CHECK_STR(ev.text, "D-Modem 1.0.0");
	CHECK_STR(ev.result, "OK");
	CHECK_STR(term(s), "ATI3\r\r\nD-Modem 1.0.0\r\n\r\nOK\r\n");
	/* the quiet one: written now, kept off the screen */
	CHECK_STR(out(s), "ATI0\r");
	feed(s, "ATI0\r\r\n56000\r\nRING\r\nOK\r\n");
	CHECK(next_of(s, DMC_EV_RESPONSE, &ev));
	CHECK(ev.tag == tag2);
	CHECK_STR(ev.text, "56000");
	CHECK_STR(term(s), "\r\nRING\r\n");
	CHECK(dmc_state(s) == DMC_RINGING);
	/* ERROR */
	tag = dmc_command(s, "ATX9", 0, 0);
	out(s);
	feed(s, "ATX9\r\r\nERROR\r\n");
	CHECK(next_of(s, DMC_EV_RESPONSE, &ev));
	CHECK(ev.tag == tag && !ev.ok);
	CHECK_STR(ev.result, "ERROR");
	/* a timeout */
	dmc_poll(s, 1000);
	tag = dmc_command(s, "ATI4", 0, 0);
	out(s);
	CHECK(dmc_poll(s, 1000) > 0);
	dmc_poll(s, 7000);
	CHECK(next_of(s, DMC_EV_RESPONSE, &ev));
	CHECK(ev.tag == tag && !ev.ok);
	CHECK_STR(ev.result, "timeout");
	dmc_close(s);
	CHECK(dmc_state(s) == DMC_CLOSED);
	dmc_session_free(s);
}

/* a call: dial, connect, online data, hang up through the escape */
static void test_call(void)
{
	dmc_session *s = dmc_session_new();
	dmc_event ev;
	dmc_open(s, 0);
	int tag = dmc_dial(s, "555-1234");
	CHECK_STR(out(s), "ATD555-1234\r");
	CHECK(dmc_state(s) == DMC_DIALING);
	/* no other command while dialling */
	int later = dmc_command(s, "ATI11", DMC_QUIET, 0);
	CHECK_STR(out(s), "");
	feed(s, "ATD555-1234\r\r\nCONNECT 33600/ARQ/V34/LAPM/V42BIS\r\n");
	CHECK(next_of(s, DMC_EV_RESPONSE, &ev));
	CHECK(ev.tag == tag && ev.ok);
	CHECK(dmc_state(s) == DMC_ONLINE);
	CHECK(dmc_connect_rate(s) == 33600);
	CHECK_STR(dmc_connect_text(s), "CONNECT 33600/ARQ/V34/LAPM/V42BIS");
	/* online: data, not results; no commands */
	feed(s, "OK\r\nERROR\r\nhello\r\n");
	CHECK(dmc_state(s) == DMC_ONLINE);
	CHECK(dmc_command(s, "ATI0", 0, 0) == -1);
	/* hang up: the guard time, +++, OK, ATH */
	dmc_poll(s, 100);
	dmc_hangup(s);
	CHECK(dmc_state(s) == DMC_HANGING_UP);
	CHECK_STR(out(s), "");
	dmc_poll(s, 1300);
	CHECK_STR(out(s), "+++");
	feed(s, "\r\nOK\r\n");
	dmc_poll(s, 1400);
	CHECK_STR(out(s), "ATH\r");
	feed(s, "ATH\r\r\nOK\r\n");
	CHECK(dmc_state(s) == DMC_IDLE);
	/* the command that waited goes now */
	CHECK_STR(out(s), "ATI11\r");
	feed(s, "ATI11\r\r\nLast call: 33600\r\nOK\r\n");
	CHECK(next_of(s, DMC_EV_RESPONSE, &ev));
	CHECK(ev.tag == later && ev.ok);
	dmc_session_free(s);
}

/* the far end hangs up; a call comes in with caller ID and is
   answered; a dial stopped */
static void test_unsolicited(void)
{
	dmc_session *s = dmc_session_new();
	dmc_event ev;
	dmc_open(s, 0);
	dmc_poll(s, 10);
	feed(s, "\r\nRING\r\n\r\nDATE = 1106\r\nTIME = 1316\r\n"
	     "NMBR = 5551234\r\nNAME = D-Modem BBS\r\n\r\nRING\r\n");
	CHECK(dmc_state(s) == DMC_RINGING);
	CHECK_STR(dmc_caller_number(s), "5551234");
	CHECK_STR(dmc_caller_name(s), "D-Modem BBS");
	CHECK(next_of(s, DMC_EV_CALLER, &ev));
	CHECK_STR(ev.text, "DATE=1106");
	/* no more rings: idle again */
	dmc_poll(s, 9000);
	CHECK(dmc_state(s) == DMC_IDLE);
	feed(s, "\r\nRING\r\n");
	int tag = dmc_answer(s);
	CHECK_STR(out(s), "ATA\r");
	CHECK(dmc_state(s) == DMC_ANSWERING);
	feed(s, "ATA\r\r\nCONNECT 28800\r\n");
	CHECK(next_of(s, DMC_EV_RESPONSE, &ev));
	CHECK(ev.tag == tag);
	CHECK(dmc_state(s) == DMC_ONLINE);
	feed(s, "data\r\nNO CARRIER\r\n");
	CHECK(dmc_state(s) == DMC_IDLE);
	/* an automatic answer (S0) */
	feed(s, "\r\nRING\r\n\r\nCONNECT 9600\r\n");
	CHECK(dmc_state(s) == DMC_ONLINE);
	feed(s, "\r\nNO CARRIER\r\n");
	CHECK(dmc_state(s) == DMC_IDLE);
	/* a dial stopped by a key */
	dmc_dial(s, "1");
	out(s);
	dmc_hangup(s);
	CHECK_STR(out(s), "\r");
	feed(s, "ATD1\r\r\nNO CARRIER\r\n");
	CHECK(dmc_state(s) == DMC_IDLE);
	dmc_session_free(s);
}

/* what the user types: ATD, the configurator */
static void test_typed(void)
{
	dmc_session *s = dmc_session_new();
	dmc_open(s, 0);
	dmc_type(s, "atdt12", 6);
	dmc_type(s, "3\r", 2);
	CHECK_STR(out(s), "atdt123\r");
	CHECK(dmc_state(s) == DMC_DIALING);
	feed(s, "atdt123\r\r\nBUSY\r\n");
	CHECK(dmc_state(s) == DMC_IDLE);
	dmc_type(s, "AT!CF\r", 6);
	out(s);
	CHECK(dmc_state(s) == DMC_CONFIGURATOR);
	feed(s, "AT!CF\r\x1b[6n\x1b[0m\x1b[2J\x1b[1;1HD-Modem setup\r\n");
	CHECK(dmc_state(s) == DMC_CONFIGURATOR);
	feed(s, "\x1b[0m\x1b[2J\x1b[H\x1b[?25h\r\nOK\r\n");
	CHECK(dmc_state(s) == DMC_IDLE);
	/* its screens as one line longer than a line is kept, the OK at the
	   end of it (as AT!CF's Alt-X sends it) */
	dmc_type(s, "AT!CF\r", 6);
	out(s);
	CHECK(dmc_state(s) == DMC_CONFIGURATOR);
	for (int i = 0; i < 1000; i++)
		feed(s, "\x1b[0;30;47m\xe2\x96\x91");
	CHECK(dmc_state(s) == DMC_CONFIGURATOR);
	feed(s, "\x1b[0m\x1b[2J\x1b[H\x1b[?25hOK\r\n");
	CHECK(dmc_state(s) == DMC_IDLE);
	/* refused (off hook) */
	dmc_type(s, "AT!CT\r", 6);
	CHECK(dmc_state(s) == DMC_CONFIGURATOR);
	feed(s, "AT!CT\r\r\nERROR\r\n");
	CHECK(dmc_state(s) == DMC_IDLE);
	/* the configurator from a command */
	dmc_event ev;
	int tag = dmc_command(s, "AT!CT", 0, 60000);
	out(s);
	CHECK(dmc_state(s) == DMC_CONFIGURATOR);
	feed(s, "AT!CT\r\r\ndmodem#exit\r\n\r\nOK\r\n");
	CHECK(next_of(s, DMC_EV_RESPONSE, &ev));
	CHECK(ev.tag == tag && ev.ok);
	CHECK(dmc_state(s) == DMC_IDLE);
	dmc_session_free(s);
}

static void test_model(const char *dir)
{
	char *text = read_file(dir, "model.txt");
	/* the response as the session gives it: no echo, no OK, LF */
	dmc_session *s = dmc_session_new();
	dmc_event ev;
	dmc_open(s, 0);
	dmc_command(s, "AT!M", DMC_QUIET, 0);
	out(s);
	feed(s, text);
	CHECK(next_of(s, DMC_EV_RESPONSE, &ev));
	CHECK(ev.ok);
	CHECK_STR(term(s), "");
	dmc_model *m = dmc_model_parse(ev.text);
	CHECK(m != NULL);
	CHECK(dmc_model_count(m) == 73);
	int c = dmc_model_find(m, "country");
	CHECK(c == 0);
	CHECK(dmc_setting_kind(m, c) == DMC_PICK);
	CHECK_STR(dmc_setting_value(m, c), "B5");
	CHECK_STR(dmc_setting_group(m, c), "Line");
	CHECK(dmc_setting_choices(m, c) > 40);
	int de = -1;
	for (int i = 0; i < dmc_setting_choices(m, c); i++)
		if (!strcmp(dmc_choice_name(m, c, i), "germany"))
			de = i;
	CHECK(de >= 0);
	CHECK_STR(dmc_setting_command(m, c, dmc_choice_value(m, c, de)),
		  "AT+GCI=42");
	CHECK_STR(dmc_setting_check(m, c, "ZZ"), "Not one of the choices");
	CHECK(dmc_setting_choice_of(m, c, "b5") >= 0);
	int a = dmc_model_find(m, "auto-answer");
	CHECK(a >= 0);
	CHECK(dmc_setting_kind(m, a) == DMC_NUMBER);
	CHECK(dmc_setting_lo(m, a) == 0 && dmc_setting_hi(m, a) == 255);
	CHECK_STR(dmc_setting_check(m, a, "2"), "");
	CHECK_STR(dmc_setting_check(m, a, "256"), "From 0 to 255");
	CHECK_STR(dmc_setting_check(m, a, "x"), "Enter a number");
	CHECK_STR(dmc_setting_command(m, a, "2"), "ATS0=2");
	CHECK_STR(dmc_setting_command(m, a, "300"), "");
	dmc_setting_set(m, a, "2");
	CHECK_STR(dmc_setting_value(m, a), "2");
	CHECK(dmc_model_groups(m) >= 4);
	CHECK_STR(dmc_model_group(m, 0), "Line");
	int text_kind = 0;
	for (int i = 0; i < dmc_model_count(m); i++)
		if (dmc_setting_kind(m, i) == DMC_TEXT) {
			text_kind = 1;
			CHECK_STR(dmc_setting_check(m, i, "1,0,1"), "");
			CHECK(*dmc_setting_check(m, i, "a"));
		}
	CHECK(text_kind);
	dmc_model_free(m);
	CHECK(dmc_model_parse("OK") == NULL);
	CHECK(dmc_model_parse("model\t2\t0") == NULL);
	free(text);
	dmc_session_free(s);
}

static void test_diag(const char *dir)
{
	char *text = read_file(dir, "diag-none.txt");
	dmc_diag *d = dmc_diag_parse(text);
	CHECK(dmc_diag_count(d) == 3);
	CHECK(!dmc_diag_has_call(d));
	CHECK_STR(dmc_diag_label(d, 0), "Report version");
	CHECK_STR(dmc_diag_value(d, 0), "1.0");
	CHECK_STR(dmc_diag_value(d, 2), "No previous call");
	dmc_diag_free(d);
	free(text);

	d = dmc_diag_parse(
		"DIAG <2A4D3263 0=10>\nDIAG <2A4D3263 1=7>\n"
		"DIAG <2A4D3263 2=0 3=0>\n"
		"DIAG <2A4D3263 4=\"E0C14513\">\n"
		"DIAG <2A4D3263 10=0C>\nDIAG <2A4D3263 20=C>\n"
		"DIAG <2A4D3263 22=0D05>\n"
		"DIAG <2A4D3263 26=8340>\nDIAG <2A4D3263 34=7080>\n"
		"DIAG <2A4D3263 40=1>\nDIAG <2A4D3263 44=81>\n"
		"DIAG <2A4D3263 52=0001E240>\nDIAG <2A4D3263 60=41>\n"
		"DIAG <2A4D3263 9A=1>\n");
	CHECK(dmc_diag_has_call(d));
	CHECK(dmc_diag_count(d) == 15);
	const char *want[][2] = {
		{ "Call setup", "Data answer tone heard" },
		{ "Mode", "Data" },
		{ "DTE interface", "Asynchronous" },
		{ "V.8 CM", "E0 C1 45 13" },
		{ "Received level", "-12 dBm" },
		{ "Transmit modulation", "V.34" },
		{ "Transmit symbol rate", "3333 baud" },
		{ "First transmit rate", "33,600 bit/s" },
		{ "Last transmit rate", "28,800 bit/s" },
		{ "Error control", "V.42 LAPM" },
		{ "Compression", "V.44" },
		{ "Characters sent", "123,456" },
		{ "Why the call ended", "The other modem hung up" },
		{ "Key 9A", "1" },
	};
	for (size_t w = 0; w < sizeof(want) / sizeof(want[0]); w++) {
		int found = 0;
		for (int i = 0; i < dmc_diag_count(d); i++)
			if (!strcmp(dmc_diag_label(d, i), want[w][0])) {
				CHECK_STR(dmc_diag_value(d, i), want[w][1]);
				found = 1;
			}
		if (!found)
			fprintf(stderr, "no row %s\n", want[w][0]);
		CHECK(found);
	}
	CHECK_STR(dmc_diag_raw(d, 0x60), "41");
	CHECK(dmc_diag_raw(d, 0x61) == NULL);
	dmc_diag_free(d);
}

static void test_vt(void)
{
	dmc_vt *t = dmc_vt_new(20, 5, 100);
	const char *s = "ATI3\r\r\nD-Modem\r\n\r\nOK\r\n";
	dmc_vt_feed(t, s, (int)strlen(s));
	CHECK(dmc_vt_changed(t));
	CHECK(!dmc_vt_changed(t));
	CHECK_STR(dmc_vt_text(t, 0), "ATI3");
	CHECK_STR(dmc_vt_text(t, 1), "D-Modem");
	CHECK_STR(dmc_vt_text(t, 3), "OK");
	CHECK(dmc_vt_cursor_row(t) == 4 && dmc_vt_cursor_col(t) == 0);
	/* scrolling into the scrollback */
	s = "a\r\nb\r\nc\r\n";
	dmc_vt_feed(t, s, (int)strlen(s));
	CHECK(dmc_vt_history(t) == 3);
	CHECK_STR(dmc_vt_text(t, -3), "ATI3");
	CHECK_STR(dmc_vt_text(t, -1), "");
	CHECK_STR(dmc_vt_text(t, 0), "OK");
	/* the configurator's way: clear, move, colours, UTF-8 */
	s = "\x1b[0m\x1b[?25l\x1b[2J\x1b[2;3H\x1b[0;1;37;44m\xe2\x94\x8c"
	    "Line\x1b[0;30;47m\xc3\xa9";
	dmc_vt_feed(t, s, (int)strlen(s));
	CHECK(!dmc_vt_cursor_visible(t));
	CHECK(dmc_vt_char(t, 1, 2) == 0x250C);
	CHECK(dmc_vt_fg(t, 1, 2) == 7 && dmc_vt_bg(t, 1, 2) == 4);
	CHECK(dmc_vt_attr(t, 1, 2) == DMC_BOLD);
	CHECK(dmc_vt_char(t, 1, 7) == 0xE9);
	CHECK(dmc_vt_fg(t, 1, 7) == 0 && dmc_vt_bg(t, 1, 7) == 7);
	CHECK_STR(dmc_vt_text(t, 1), "  \xe2\x94\x8cLine\xc3\xa9");
	/* the size probe: to the far corner and asked where */
	char r[32];
	s = "\x1b[999;999H\x1b[6n";
	dmc_vt_feed(t, s, (int)strlen(s));
	int n = dmc_vt_reply(t, r, sizeof(r) - 1);
	r[n] = '\0';
	CHECK_STR(r, "\x1b[5;20R");
	/* and the UTF-8 probe: CR, é, asked again: column 2 */
	s = "\r\xc3\xa9\x1b[6n";
	dmc_vt_feed(t, s, (int)strlen(s));
	n = dmc_vt_reply(t, r, sizeof(r) - 1);
	r[n] = '\0';
	CHECK_STR(r, "\x1b[5;2R");
	/* erase to the end of the line, insert, delete */
	s = "\x1b[3;1Habcdef\x1b[3;3H\x1b[K";
	dmc_vt_feed(t, s, (int)strlen(s));
	CHECK_STR(dmc_vt_text(t, 2), "ab");
	s = "\x1b[3;1H\x1b[2@";
	dmc_vt_feed(t, s, (int)strlen(s));
	CHECK_STR(dmc_vt_text(t, 2), "  ab");
	s = "\x1b[3P";
	dmc_vt_feed(t, s, (int)strlen(s));
	CHECK_STR(dmc_vt_text(t, 2), "b");
	/* a wrapped line */
	s = "\x1b[4;1H01234567890123456789X";
	dmc_vt_feed(t, s, (int)strlen(s));
	CHECK_STR(dmc_vt_text(t, 3), "01234567890123456789");
	CHECK_STR(dmc_vt_text(t, 4), "X");
	/* keys */
	n = dmc_vt_key(t, DMC_KEY_F2, r, sizeof(r));
	CHECK(n == 3 && !memcmp(r, "\x1bOQ", 3));
	n = dmc_vt_key(t, DMC_KEY_UP, r, sizeof(r));
	CHECK(n == 3 && !memcmp(r, "\x1b[A", 3));
	/* resized: the scrollback's lines come back over the screen */
	n = dmc_vt_history(t);
	dmc_vt_resize(t, 80, 24);
	CHECK(dmc_vt_cols(t) == 80 && dmc_vt_rows(t) == 24);
	CHECK(dmc_vt_history(t) == 0);
	CHECK_STR(dmc_vt_text(t, 3 + n), "01234567890123456789");
	CHECK(dmc_vt_palette(4) == 0x0000AA);
	dmc_vt_free(t);
}

/* feed a string */
static void vfeed(dmc_vt *t, const char *s)
{
	dmc_vt_feed(t, s, (int)strlen(s));
}

static const char *vreply(dmc_vt *t)
{
	static char r[256];
	int n = dmc_vt_reply(t, r, sizeof(r) - 1);
	r[n] = '\0';
	return r;
}

/* a row's text with the screen's blanks */
static int has_image(const dmc_vt *t)
{
	int y, x;
	dmc_cell c;
	for (y = 0; y < dmc_vt_rows(t); y++)
		for (x = 0; x < dmc_vt_cols(t); x++)
			if (dmc_vt_cell(t, y, x, &c) && (c.flags & DMC_CELL_IMAGE))
				return 1;
	return 0;
}

/* every profile given every octet and noise, every cell read: nothing
   breaks (run under ASan and UBSan in CI) */
static void test_vt_noise(void)
{
	int i, k, y, x, n;
	unsigned seed = 12345;
	char b[4096], r[64];
	dmc_cell c;
	for (i = 0; i < dmc_vt_profile_count(); i++) {
		dmc_vt *t = dmc_vt_new(80, 25, 50);
		const char *id = dmc_vt_profile_id(i);
		CHECK(dmc_vt_set_profile(t, id));
		CHECK_STR(dmc_vt_get_profile(t), id);
		dmc_vt_set_cell_pixels(t, 8, 16);
		for (k = 0; k < 256; k++)
			b[k] = (char)k;
		dmc_vt_feed(t, b, 256);
		for (k = 0; k < 40; k++) {
			for (n = 0; n < (int)sizeof(b); n++) {
				seed = seed * 1103515245u + 12345u;
				b[n] = (char)(seed >> 16);
				/* plenty of ESC, CSI's and digits */
				if ((seed >> 8) % 7 == 0)
					b[n] = "\x1b[;0123456789!|\r\n"[(seed >> 4) % 18];
			}
			dmc_vt_feed(t, b, n);
			if (k == 20)
				dmc_vt_resize(t, 40 + k, 20);
		}
		for (y = -dmc_vt_history(t); y < dmc_vt_rows(t); y++)
			for (x = 0; x < dmc_vt_cols(t); x++) {
				dmc_vt_cell(t, y, x, &c);
				dmc_vt_cell_text(t, y, x);
			}
		for (k = DMC_KEY_UP; k <= DMC_KEY_KP_ENTER; k++)
			dmc_vt_key_mods(t, k, k % 8, r, sizeof(r));
		for (k = 0; k < 0x180; k++)
			dmc_vt_char_bytes(t, (uint32_t)k, k % 8, r, sizeof(r));
		dmc_vt_tap(t, 0.5, 0.5, r, sizeof(r));
		while (dmc_vt_reply(t, r, sizeof(r)) > 0)
			;
		dmc_vt_reset(t);
		dmc_vt_free(t);
	}
}

static void test_vt_profiles(void)
{
	dmc_vt *t = dmc_vt_new(80, 25, 100);
	dmc_cell c;
	char r[64];
	int n, w, h;

	CHECK(dmc_vt_profile_count() == 29);
	CHECK_STR(dmc_vt_profile_id(0), "xterm");
	CHECK_STR(dmc_vt_profile_term("rip"), "rip");
	CHECK_STR(dmc_vt_profile_telnet("rip"), "ANSI");
	CHECK_STR(dmc_vt_profile_telnet("vt220"), "DEC-VT220");
	CHECK_STR(dmc_vt_profile_term("nonsense"), "xterm-256color");
	CHECK(!dmc_vt_set_profile(t, "nonsense"));

	/* xterm and the VTs' answers */
	CHECK_STR(dmc_vt_get_profile(t), "xterm");
	vfeed(t, "\x1b[c");
	CHECK_STR(vreply(t), "\x1b[?62;4;22c");
	dmc_vt_set_profile(t, "vt100");
	vfeed(t, "\x1b[c");
	CHECK_STR(vreply(t), "\x1b[?1;2c");
	dmc_vt_set_profile(t, "vt420");
	vfeed(t, "\x1b[>c");
	CHECK_STR(vreply(t), "\x1b[>41;10;0c");
	/* a VT420's rectangles: DECFRA */
	vfeed(t, "\x1b[2J\x1b[42;2;2;3;4$x");
	CHECK_STR(dmc_vt_text(t, 1), " ***");
	CHECK_STR(dmc_vt_text(t, 2), " ***");

	/* VT52 */
	dmc_vt_set_profile(t, "vt52");
	vfeed(t, "\x1bH\x1bJ\x1bY\"%X\x1bZ");
	CHECK(dmc_vt_char(t, 2, 5) == 'X');
	CHECK_STR(vreply(t), "\x1b/K");

	/* ANSI-BBS: code page 437, the VGA's font, ESC [2J homing */
	dmc_vt_set_profile(t, "ansi-bbs");
	CHECK(dmc_vt_glyph_cell(t, &w, &h) && w == 8 && h == 16);
	vfeed(t, "\x1b[5;5H\x1b[2J\xdb\xb0");
	CHECK(dmc_vt_char(t, 0, 0) == 0x2588 && dmc_vt_char(t, 0, 1) == 0x2591);
	CHECK(dmc_vt_glyph(t, 0x2588, &w, &h) && h == 16);
	CHECK(dmc_vt_glyph_row(t, 0x2588, 3) == 0xff);
	/* SyncTERM's Topaz (42) at the VGA's height, then back */
	n = (int)dmc_vt_glyph_row(t, 'A', 4);
	vfeed(t, "\x1b[0;42 D");
	CHECK(dmc_vt_glyph(t, 'A', &w, &h) && h == 16);
	CHECK((int)dmc_vt_glyph_row(t, 'A', 4) != n);
	vfeed(t, "\x1b[0;0 D");
	CHECK((int)dmc_vt_glyph_row(t, 'A', 4) == n);
	/* the line speed */
	vfeed(t, "\x1b[0;6*r");
	CHECK(dmc_vt_emulated_bps(t) > 0);

	/* Amiga: Topaz, 8x8, its colours */
	dmc_vt_set_profile(t, "amiga");
	CHECK(dmc_vt_glyph_cell(t, &w, &h) && w == 8 && h == 8);
	vfeed(t, "\x1b[H\x1b[2J\x1b[31mR");
	CHECK(dmc_vt_cell(t, 0, 0, &c) && c.fg == 0xcc2222);

	/* Avatar: ^V ^A attribute */
	dmc_vt_set_profile(t, "avatar");
	vfeed(t, "\x0c\x16\x01\x1fX");
	CHECK(dmc_vt_char(t, 0, 0) == 'X');
	CHECK(dmc_vt_cell(t, 0, 0, &c) && c.fg == 0xffffff && c.bg == dmc_vt_palette(4));

	/* RIPscrip */
	dmc_vt_set_profile(t, "rip");
	dmc_vt_set_cell_pixels(t, 8, 8);
	CHECK(dmc_vt_requested_rows(t) == 43);
	vfeed(t, "\x1b[!");
	CHECK_STR(vreply(t), "RIPSCRIP015400");
	vfeed(t, "\x1b[2JHi!x\r\n!x\r\n");
	CHECK_STR(dmc_vt_text(t, 0), "Hi!x");
	CHECK_STR(dmc_vt_text(t, 1), "!x");
	CHECK(!has_image(t));
	vfeed(t, "!|c0F|L00000A0A\r\n");
	CHECK(has_image(t));
	CHECK_STR(dmc_vt_text(t, 2), "");
	vfeed(t, "!|1M0000000Z0Z00000000HI^M\r\n");
	n = dmc_vt_tap(t, 0.01, 0.01, r, sizeof(r));
	CHECK(n == 3 && !memcmp(r, "HI\r", 3));
	CHECK(dmc_vt_tap(t, 0.99, 0.99, r, sizeof(r)) == 0);

	/* a Commodore 64 */
	dmc_vt_set_profile(t, "c64");
	CHECK(dmc_vt_requested_cols(t) == 40 && dmc_vt_requested_rows(t) == 25);
	CHECK(dmc_vt_glyph_cell(t, &w, &h) && w == 8 && h == 8);
	vfeed(t, "\x93" "A\x1c" "B\x0e" "C");
	CHECK(dmc_vt_char(t, 0, 0) == 'A');
	CHECK(dmc_vt_cell(t, 0, 1, &c) && c.fg == 0x68372b);
	CHECK(dmc_vt_char(t, 0, 2) == 'c');
	n = dmc_vt_char_bytes(t, 'a', 0, r, sizeof(r));
	CHECK(n == 1 && r[0] == 0x41);
	n = dmc_vt_key(t, DMC_KEY_UP, r, sizeof(r));
	CHECK(n == 1 && (unsigned char)r[0] == 0x91);

	/* an Atari 8-bit */
	dmc_vt_set_profile(t, "atari");
	vfeed(t, "\x7d" "AB\x9b" "C");
	CHECK_STR(dmc_vt_text(t, 0), "AB");
	CHECK_STR(dmc_vt_text(t, 1), "C");

	/* an Atari ST: its pens */
	dmc_vt_set_profile(t, "atari-st-low");
	CHECK(dmc_vt_requested_cols(t) == 40);
	vfeed(t, "\x1b" "E\x1b" "b\x01X");
	CHECK(dmc_vt_cell(t, 0, 0, &c) && c.ch == 'X' && c.fg == 0xff0000);
	vfeed(t, "\x1bZ");
	CHECK_STR(vreply(t), "\x1b/K");

	/* Viewdata */
	dmc_vt_set_profile(t, "viewdata");
	CHECK(dmc_vt_requested_cols(t) == 40 && dmc_vt_requested_rows(t) == 24);
	vfeed(t, "\x0c" "A#");
	CHECK(dmc_vt_char(t, 0, 0) == 'A' && dmc_vt_char(t, 0, 1) == 0xa3);

	/* the Minitel: its first row is the status row */
	dmc_vt_set_profile(t, "minitel");
	vfeed(t, "\x0c" "M");
	CHECK(dmc_vt_char(t, 1, 0) == 'M');

	/* sixel on xterm */
	dmc_vt_set_profile(t, "xterm");
	dmc_vt_set_cell_pixels(t, 8, 16);
	vfeed(t, "\x1bPq#0;2;100;0;0#0~~~~~~~~\x1b\\");
	CHECK(has_image(t));

	/* ReGIS on a VT340 */
	dmc_vt_set_profile(t, "vt340");
	vfeed(t, "\x1b[2J");
	CHECK(!has_image(t));
	vfeed(t, "\x1bP0pS(E)P[100,100]V[200,200]\x1b\\");
	CHECK(has_image(t));

	/* a Tektronix 4014 */
	dmc_vt_set_profile(t, "tek4014");
	vfeed(t, "\x1d\x20\x60\x20\x40\x3f\x7f\x3f\x5f");
	CHECK(has_image(t));

	/* a reset keeps the profile */
	dmc_vt_reset(t);
	CHECK_STR(dmc_vt_get_profile(t), "tek4014");
	dmc_vt_free(t);
}

static void test_daemon(void)
{
	char b[256];
	CHECK(dmc_daemon_ready("Use `/dev/ttyDM0' as modem device, Ctrl+C "
			       "for termination.", b, sizeof(b)));
	CHECK_STR(b, "/dev/ttyDM0");
	CHECK(dmc_daemon_ready("Use `/dev/pts/3' as modem device, Ctrl+C for "
			       "termination.", b, sizeof(b)));
	CHECK_STR(b, "/dev/pts/3");
	CHECK(dmc_daemon_ready("Use TCP 127.0.0.1:2323 as the modem, Ctrl+C "
			       "for termination.", b, sizeof(b)));
	CHECK_STR(b, "TCP 127.0.0.1:2323");
	CHECK(!dmc_daemon_ready("modem `dmodem0' created. TTY is `/dev/pts/3'",
				b, sizeof(b)));
	CHECK(dmc_daemon_error("error: cannot create PTY."));
	CHECK(!dmc_daemon_error("13:21:22.774 sip: SIP on 127.0.0.1:5060"));
}

/* the session's bytes to the modem, binary: how many into b */
static int outb(dmc_session *s, unsigned char *b, int max)
{
	return dmc_output(s, (char *)b, max);
}

static void feedb(dmc_session *s, const unsigned char *b, int n)
{
	dmc_feed(s, (const char *)b, n);
}

/* the setup of a voice call: voice mode, mu-law, then cmd and its
   result; the stream started */
static void voice_setup(dmc_session *s, const char *cmd, int state)
{
	CHECK_STR(out(s), "AT+FCLASS=8\r");
	feed(s, "AT+FCLASS=8\r\r\nOK\r\n");
	CHECK_STR(out(s), "AT+VSM=4\r");
	feed(s, "AT+VSM=4\r\r\nOK\r\n");
	char want[64];
	snprintf(want, sizeof(want), "%s\r", cmd);
	CHECK_STR(out(s), want);
	snprintf(want, sizeof(want), "%s\r\r\nOK\r\n", cmd);
	feed(s, want);
	CHECK(dmc_state(s) == state);
	CHECK_STR(out(s), "AT+VTR\r");
	term(s);
	feed(s, "AT+VTR\r\r\nCONNECT\r\n");
	/* (none of it on the screen) */
	CHECK_STR(term(s), "");
}

/* a voice call dialled: the samples both ways, DTMF, hanging up */
static void test_voice(void)
{
	dmc_session *s = dmc_session_new();
	dmc_event ev;
	static unsigned char b[65536];
	int16_t pcm[256];
	dmc_open(s, 0);
	int tag = dmc_voice_dial(s, "555-1000");
	CHECK(tag > 0);
	CHECK(dmc_voice_dial(s, "1") == -1);
	CHECK(dmc_command(s, "ATI3", 0, 0) == -1);
	CHECK_STR(out(s), "AT+FCLASS=8\r");
	feed(s, "AT+FCLASS=8\r\r\nOK\r\n");
	CHECK_STR(out(s), "AT+VSM=4\r");
	feed(s, "AT+VSM=4\r\r\nOK\r\n");
	CHECK_STR(out(s), "ATD555-1000\r");
	CHECK(dmc_state(s) == DMC_DIALING);
	feed(s, "ATD555-1000\r\r\nOK\r\n");
	CHECK(next_of(s, DMC_EV_RESPONSE, &ev));
	CHECK(ev.tag == tag && ev.ok);
	CHECK(dmc_state(s) == DMC_VOICE);
	CHECK_STR(dmc_state_name(DMC_VOICE), "Voice call");
	CHECK_STR(out(s), "AT+VTR\r");
	term(s);
	/* the stream: samples (a DLE among them doubled), a digit */
	unsigned char in[] = { 'C', 'O', 'N', 'N', 'E', 'C', 'T', '\r', '\n',
			       0xff, 0x80, 0x10, 0x10, 0x00, 0x10, '5',
			       0x10, '~', 0x7f };
	feedb(s, in, sizeof(in));
	CHECK_STR(term(s), "");
	int n = dmc_voice_read(s, pcm, 256);
	CHECK(n == 5);
	CHECK(pcm[0] == 0);
	CHECK(pcm[1] == 32124);
	CHECK(pcm[3] == -32124);
	CHECK(pcm[4] == 0);
	CHECK(next_of(s, DMC_EV_VOICE, &ev));
	CHECK_STR(ev.text, "5");
	CHECK_STR(ev.result, "digit");
	CHECK(!next_of(s, DMC_EV_VOICE, &ev));
	/* ours: silence, 60 ms ahead, then at the line's pace */
	CHECK(dmc_poll(s, 0) == 20);
	n = outb(s, b, sizeof(b));
	CHECK(n == 480);
	CHECK(b[0] == 0xff && b[479] == 0xff);
	for (int i = 0; i < 160; i++)
		pcm[i] = 8000;
	dmc_voice_write(s, pcm, 160);
	dmc_poll(s, 20);
	n = outb(s, b, sizeof(b));
	CHECK(n == 160);
	CHECK(b[0] != 0xff && b[0] == b[159]);
	/* a sample that is a DLE: doubled */
	pcm[0] = -15996;
	dmc_voice_write(s, pcm, 1);
	dmc_poll(s, 21);
	n = outb(s, b, sizeof(b));
	CHECK(n == 9 && b[0] == 0x10 && b[1] == 0x10);
	/* a DTMF digit: its tone in our samples */
	CHECK(dmc_voice_dtmf(s, '!') == -1);
	CHECK(dmc_voice_dtmf(s, '1') == 0);
	dmc_poll(s, 41);
	n = outb(s, b, sizeof(b));
	int tone = 0;
	for (int i = 0; i < n; i++)
		if (b[i] != 0xff)
			tone++;
	CHECK(n == 160 && tone > 100);
	/* hung up here: <DLE>^, the modem's <DLE><ETX> and OK, ATH */
	dmc_hangup(s);
	n = outb(s, b, sizeof(b));
	CHECK(n == 2 && b[0] == 0x10 && b[1] == '^');
	CHECK(dmc_state(s) == DMC_HANGING_UP);
	unsigned char end[] = { 0xff, 0x10, 0x03, '\r', '\n', 'O', 'K', '\r',
				'\n' };
	feedb(s, end, sizeof(end));
	CHECK_STR(out(s), "ATH\r");
	feed(s, "ATH\r\r\nOK\r\n");
	CHECK(dmc_state(s) == DMC_IDLE);
	CHECK(dmc_poll(s, 100) == -1);

	/* the far end hangs up: <DLE>l, then the stream ended as before */
	tag = dmc_voice_dial(s, "1");
	CHECK_STR(out(s), "AT+FCLASS=8\r");
	feed(s, "AT+FCLASS=8\r\r\nOK\r\nAT+VSM=4\r\r\nOK\r\n");
	CHECK_STR(out(s), "AT+VSM=4\rATD1\r");
	feed(s, "ATD1\r\r\nOK\r\nAT+VTR\r\r\nCONNECT\r\n");
	CHECK(dmc_state(s) == DMC_VOICE);
	unsigned char lci[] = { 0x10, 'l' };
	feedb(s, lci, 2);
	CHECK(next_of(s, DMC_EV_VOICE, &ev));
	CHECK_STR(ev.result, "hangup");
	n = outb(s, b, sizeof(b));
	CHECK(n >= 2 && b[n - 2] == 0x10 && b[n - 1] == '^');
	feedb(s, end + 1, sizeof(end) - 1);
	CHECK_STR(out(s), "ATH\r");
	feed(s, "ATH\r\r\nOK\r\n");
	CHECK(dmc_state(s) == DMC_IDLE);

	/* busy: the call given up, the modem on hook in data mode */
	tag = dmc_voice_dial(s, "2");
	out(s);
	feed(s, "AT+FCLASS=8\r\r\nOK\r\nAT+VSM=4\r\r\nOK\r\n");
	out(s);
	feed(s, "ATD2\r\r\nBUSY\r\n");
	CHECK(next_of(s, DMC_EV_RESPONSE, &ev));
	CHECK(ev.tag == tag && !ev.ok);
	CHECK_STR(ev.result, "BUSY");
	CHECK(dmc_state(s) == DMC_IDLE);
	CHECK_STR(out(s), "ATH\r");
	feed(s, "ATH\r\r\nOK\r\n");
	CHECK(dmc_state(s) == DMC_IDLE);

	/* a modem with no voice mode: the call refused */
	tag = dmc_voice_dial(s, "3");
	out(s);
	feed(s, "AT+FCLASS=8\r\r\nERROR\r\n");
	CHECK(next_of(s, DMC_EV_RESPONSE, &ev));
	CHECK(ev.tag == tag && !ev.ok);
	CHECK_STR(out(s), "ATH\r");
	feed(s, "ATH\r\r\nOK\r\n");
	CHECK(dmc_command(s, "ATI3", 0, 0) > 0);
	CHECK_STR(out(s), "ATI3\r");
	feed(s, "ATI3\r\r\nOK\r\n");

	/* answered: RING, AT+VLS=1 */
	CHECK(dmc_voice_answer(s) == -1);
	feed(s, "RING\r\n");
	CHECK(dmc_ring_count(s) == 1);
	tag = dmc_voice_answer(s);
	CHECK(tag > 0);
	voice_setup(s, "AT+VLS=1", DMC_VOICE);
	dmc_close(s);
	CHECK(dmc_state(s) == DMC_CLOSED);
	dmc_session_free(s);
}

/* the answering machine: answers after two RINGs, the greeting, the
   beep, the caller's message until they are quiet */
static void test_voicemail(void)
{
	dmc_session *s = dmc_session_new();
	dmc_event ev;
	static unsigned char b[65536];
	static int16_t greeting[800];
	for (int i = 0; i < 800; i++)
		greeting[i] = (int16_t)(i & 1 ? 6000 : -6000);
	dmc_voicemail_set(s, greeting, 800, 10, 1);
	dmc_voicemail_auto(s, 2);
	dmc_open(s, 0);
	feed(s, "RING\r\n\r\nNMBR = 5551234\r\n");
	CHECK_STR(out(s), "");
	feed(s, "RING\r\n");
	voice_setup(s, "AT+VLS=1", DMC_VOICEMAIL);
	CHECK_STR(dmc_state_name(DMC_VOICEMAIL), "Taking a message");
	/* the greeting, then the beep: 0.1 s and 0.5 s */
	dmc_poll(s, 0);
	dmc_poll(s, 1000);
	int n = outb(s, b, sizeof(b));
	CHECK(n == 8480);
	CHECK(b[0] == b[2] && b[0] != 0xff && b[1] != b[0]);
	CHECK(b[902] != 0xff);
	CHECK(b[4900] == 0xff);
	/* the caller speaks for 2 s, then is quiet for 1 s */
	static unsigned char talk[24000];
	for (int i = 0; i < 16000; i++)
		talk[i] = (unsigned char)(i & 1 ? 0x90 : 0x10 + 0x80);
	memset(talk + 16000, 0xff, 8000);
	feedb(s, talk, 24000);
	CHECK(next_of(s, DMC_EV_MESSAGE, &ev));
	CHECK(ev.ok);
	CHECK_STR(ev.text, "5551234");
	const int16_t *msg;
	n = dmc_voice_message(s, &msg);
	CHECK(n == 16000 + 4000);
	CHECK(msg[0] != 0 && msg[n - 1] == 0);
	n = outb(s, b, sizeof(b));
	CHECK(n >= 2 && b[n - 2] == 0x10 && b[n - 1] == '^');
	unsigned char end[] = { 0x10, 0x03, '\r', '\n', 'O', 'K', '\r', '\n' };
	feedb(s, end, sizeof(end));
	CHECK_STR(out(s), "ATH\r");
	feed(s, "ATH\r\r\nOK\r\n");
	CHECK(dmc_state(s) == DMC_IDLE);

	/* a caller who hangs up in the greeting: no message */
	dmc_voicemail_auto(s, 0);
	feed(s, "RING\r\n");
	CHECK_STR(out(s), "");
	CHECK(dmc_voicemail_answer(s) > 0);
	voice_setup(s, "AT+VLS=1", DMC_VOICEMAIL);
	unsigned char lci[] = { 0x10, 'l' };
	feedb(s, lci, 2);
	feedb(s, end, sizeof(end));
	out(s);
	feed(s, "ATH\r\r\nOK\r\n");
	CHECK(next_of(s, DMC_EV_MESSAGE, &ev));
	CHECK(!ev.ok);
	CHECK(dmc_state(s) == DMC_IDLE);

	/* picked up while it plays the greeting: a voice call */
	feed(s, "RING\r\n");
	CHECK(dmc_voicemail_answer(s) > 0);
	voice_setup(s, "AT+VLS=1", DMC_VOICEMAIL);
	CHECK(dmc_voice_pickup(s) == 0);
	CHECK(dmc_state(s) == DMC_VOICE);
	CHECK(dmc_voice_pickup(s) == -1);
	dmc_hangup(s);
	feedb(s, end, sizeof(end));
	out(s);
	feed(s, "ATH\r\r\nOK\r\n");
	CHECK(dmc_state(s) == DMC_IDLE);
	CHECK(!next_of(s, DMC_EV_MESSAGE, &ev));
	dmc_session_free(s);
}

/* WAV files: ours read back; 16 kHz stereo made 8 kHz mono */
static void test_wav(void)
{
	char h[44 + 8];
	int16_t *pcm;
	CHECK(dmc_wav_header(h, 4) == 44);
	CHECK(!memcmp(h, "RIFF", 4) && !memcmp(h + 36, "data", 4));
	CHECK((unsigned char)h[40] == 8);
	memcpy(h + 44, "\x10\x00\xf0\xff\x00\x40\x00\x80", 8);
	CHECK(dmc_wav_decode(h, sizeof(h), &pcm) == 4);
	CHECK(pcm[0] == 16 && pcm[1] == -16 && pcm[2] == 16384 &&
	      pcm[3] == -32768);
	dmc_free(pcm);
	/* 16 kHz, two channels, 8 frames: 4 samples */
	char w[44 + 32];
	dmc_wav_header(w, 16);
	w[22] = 2;                      /* channels */
	w[24] = (char)0x80; w[25] = 0x3e; /* 16000 */
	memset(w + 44, 0, 32);
	w[44] = 0x00; w[45] = 0x10;     /* frame 0: 4096 and 0 */
	CHECK(dmc_wav_decode(w, sizeof(w), &pcm) == 4);
	CHECK(pcm[0] == 2048 && pcm[1] == 0);
	dmc_free(pcm);
	CHECK(dmc_wav_decode("RIFF", 4, &pcm) == -1 && !pcm);
}

/* (ffmpeg -f lavfi -i sine=f=500:r=8000:d=0.07, -c:a adpcm_ima_wav or adpcm_ms -block_size 64) */
static const unsigned char ima_wav[414] = {
	0x52, 0x49, 0x46, 0x46, 0x96, 0x01, 0x00, 0x00, 0x57, 0x41, 0x56, 0x45, 0x66, 0x6d, 0x74, 0x20,
	0x14, 0x00, 0x00, 0x00, 0x11, 0x00, 0x01, 0x00, 0x40, 0x1f, 0x00, 0x00, 0x80, 0x3e, 0x00, 0x00,
	0x40, 0x00, 0x04, 0x00, 0x02, 0x00, 0x79, 0x00, 0x66, 0x61, 0x63, 0x74, 0x04, 0x00, 0x00, 0x00,
	0x30, 0x02, 0x00, 0x00, 0x4c, 0x49, 0x53, 0x54, 0x1a, 0x00, 0x00, 0x00, 0x49, 0x4e, 0x46, 0x4f,
	0x49, 0x53, 0x46, 0x54, 0x0e, 0x00, 0x00, 0x00, 0x4c, 0x61, 0x76, 0x66, 0x36, 0x30, 0x2e, 0x31,
	0x36, 0x2e, 0x31, 0x30, 0x30, 0x00, 0x64, 0x61, 0x74, 0x61, 0x40, 0x01, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x77, 0x77, 0x77, 0xf2, 0xad, 0x99, 0x21, 0x53, 0x33, 0x12, 0xb9, 0xcd, 0xbb, 0x8a,
	0x30, 0x45, 0x33, 0x12, 0xb9, 0xcd, 0xbb, 0x9a, 0x31, 0x45, 0x33, 0x12, 0xb9, 0xcd, 0xbb, 0x9a,
	0x31, 0x45, 0x33, 0x12, 0xb9, 0xcd, 0xbb, 0x9a, 0x31, 0x45, 0x33, 0x12, 0xb9, 0xcd, 0xbb, 0x9a,
	0x31, 0x45, 0x33, 0x12, 0xb9, 0xcd, 0xbb, 0x9a, 0x31, 0x45, 0x33, 0x12, 0xb9, 0xcd, 0xe1, 0xf9,
	0x39, 0x00, 0x9b, 0x19, 0x52, 0x43, 0x23, 0x80, 0xdb, 0xcb, 0xab, 0x18, 0x52, 0x43, 0x23, 0x90,
	0xda, 0xcb, 0xab, 0x18, 0x52, 0x43, 0x23, 0x90, 0xda, 0xcb, 0xab, 0x18, 0x52, 0x43, 0x23, 0x90,
	0xda, 0xcb, 0xab, 0x18, 0x52, 0x43, 0x23, 0x90, 0xda, 0xcb, 0xab, 0x18, 0x52, 0x43, 0x23, 0x90,
	0xda, 0xcb, 0xab, 0x18, 0x52, 0x43, 0x23, 0x90, 0xda, 0xcb, 0xab, 0x18, 0x52, 0x43, 0x50, 0x0b,
	0x39, 0x00, 0x02, 0xa9, 0xcc, 0xbb, 0x8a, 0x30, 0x45, 0x33, 0x12, 0xb9, 0xcd, 0xbb, 0x8a, 0x30,
	0x45, 0x33, 0x02, 0xb8, 0xcd, 0xbb, 0x8a, 0x30, 0x45, 0x33, 0x02, 0xb8, 0xcd, 0xbb, 0x8a, 0x30,
	0x45, 0x33, 0x02, 0xb8, 0xcd, 0xbb, 0x8a, 0x30, 0x45, 0x33, 0x02, 0xb8, 0xcd, 0xbb, 0x8a, 0x30,
	0x45, 0x33, 0x02, 0xb8, 0xcd, 0xbb, 0x8a, 0x30, 0x45, 0x33, 0x02, 0xb8, 0xcd, 0xbb, 0x39, 0xf1,
	0x37, 0x00, 0x08, 0x43, 0x44, 0x22, 0x80, 0xcb, 0xbc, 0xab, 0x08, 0x53, 0x34, 0x23, 0x91, 0xdb,
	0xbc, 0xab, 0x08, 0x53, 0x34, 0x23, 0x91, 0xdb, 0xbc, 0xab, 0x08, 0x53, 0x34, 0x23, 0x91, 0xdb,
	0xbc, 0xab, 0x08, 0x53, 0x34, 0x23, 0x91, 0xdb, 0xbc, 0xab, 0x08, 0x53, 0x34, 0x23, 0x91, 0xdb,
	0xbc, 0xab, 0x19, 0x53, 0x34, 0x23, 0x91, 0xdb, 0xbc, 0xab, 0x19, 0x53, 0x34, 0x23, 0xff, 0x0f,
	0x36, 0x00, 0xb8, 0xbd, 0xbc, 0x8a, 0x21, 0x35, 0x34, 0x02, 0xa9, 0xbd, 0xbc, 0x8a, 0x21, 0x35,
	0x34, 0x02, 0xa9, 0xbd, 0xbc, 0x8a, 0x21, 0x35, 0x34, 0x02, 0xa9, 0xbd, 0xbc, 0x8a, 0x21, 0x35,
	0x34, 0x02, 0xa9, 0xbd, 0xbc, 0x8a, 0x21, 0x35, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
	0x80, 0x80, 0x80, 0x80, 0x00, 0x88, 0x00, 0x88, 0x00, 0x88, 0x00, 0x88, 0x00, 0x08,
};

static const unsigned char ms_wav[444] = {
	0x52, 0x49, 0x46, 0x46, 0xb4, 0x01, 0x00, 0x00, 0x57, 0x41, 0x56, 0x45, 0x66, 0x6d, 0x74, 0x20,
	0x32, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00, 0x40, 0x1f, 0x00, 0x00, 0x80, 0x3e, 0x00, 0x00,
	0x40, 0x00, 0x04, 0x00, 0x20, 0x00, 0x74, 0x00, 0x07, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02,
	0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x40, 0x00, 0xf0, 0x00, 0x00, 0x00, 0xcc, 0x01,
	0x30, 0xff, 0x88, 0x01, 0x18, 0xff, 0x66, 0x61, 0x63, 0x74, 0x04, 0x00, 0x00, 0x00, 0x30, 0x02,
	0x00, 0x00, 0x4c, 0x49, 0x53, 0x54, 0x1a, 0x00, 0x00, 0x00, 0x49, 0x4e, 0x46, 0x4f, 0x49, 0x53,
	0x46, 0x54, 0x0e, 0x00, 0x00, 0x00, 0x4c, 0x61, 0x76, 0x66, 0x36, 0x30, 0x2e, 0x31, 0x36, 0x2e,
	0x31, 0x30, 0x30, 0x00, 0x64, 0x61, 0x74, 0x61, 0x40, 0x01, 0x00, 0x00, 0x00, 0x10, 0x00, 0x1f,
	0x06, 0x00, 0x00, 0x77, 0x76, 0xec, 0xdc, 0xdf, 0xf1, 0x25, 0x34, 0x32, 0x00, 0xdb, 0xdc, 0xde,
	0x00, 0x35, 0x34, 0x32, 0x00, 0xdb, 0xdc, 0xde, 0x00, 0x35, 0x34, 0x32, 0x00, 0xdb, 0xdc, 0xde,
	0xf1, 0x35, 0x34, 0x32, 0x1f, 0xdb, 0xdc, 0xde, 0xf1, 0x35, 0x34, 0x32, 0x1f, 0xdb, 0xdc, 0xde,
	0xf1, 0x35, 0x34, 0x32, 0x1f, 0xdb, 0xdc, 0xde, 0xf1, 0x35, 0x34, 0x32, 0x00, 0x87, 0x01, 0xc7,
	0x0e, 0xff, 0x0f, 0xec, 0xcd, 0xde, 0xf1, 0x35, 0x43, 0x32, 0x1f, 0xdb, 0xcd, 0xde, 0xf1, 0x35,
	0x43, 0x32, 0x1f, 0xdb, 0xcd, 0xde, 0xf1, 0x35, 0x43, 0x32, 0x1f, 0xdb, 0xcd, 0xde, 0xf1, 0x35,
	0x43, 0x32, 0x1f, 0xdb, 0xcd, 0xde, 0xf1, 0x35, 0x43, 0x32, 0x1f, 0xdb, 0xcd, 0xde, 0xf1, 0x35,
	0x43, 0x32, 0x1f, 0xdb, 0xcd, 0xde, 0xf1, 0x35, 0x43, 0x32, 0x1f, 0xdb, 0x00, 0x99, 0x01, 0xe1,
	0xf9, 0x00, 0x00, 0xdd, 0xf1, 0x44, 0x44, 0x22, 0x1f, 0xdc, 0xcd, 0xce, 0x00, 0x34, 0x44, 0x22,
	0x1f, 0xdc, 0xcd, 0xce, 0x01, 0x24, 0x44, 0x22, 0x1f, 0xdc, 0xcd, 0xce, 0x01, 0x24, 0x44, 0x22,
	0x1f, 0xdc, 0xcd, 0xce, 0x01, 0x24, 0x44, 0x22, 0x1f, 0xdc, 0xcd, 0xce, 0x01, 0x24, 0x44, 0x22,
	0x1f, 0xdc, 0xcd, 0xce, 0x01, 0x24, 0x44, 0x22, 0x1f, 0xdc, 0xcd, 0xce, 0x00, 0xb7, 0x01, 0x39,
	0xf1, 0x01, 0xf0, 0x23, 0x53, 0x22, 0x1f, 0xeb, 0xdc, 0xde, 0x00, 0x35, 0x34, 0x32, 0x0f, 0xeb,
	0xdc, 0xde, 0x00, 0x35, 0x34, 0x32, 0x00, 0xdb, 0xdc, 0xde, 0x00, 0x35, 0x34, 0x32, 0x00, 0xdb,
	0xdc, 0xde, 0xf1, 0x35, 0x34, 0x32, 0x1f, 0xdb, 0xdc, 0xde, 0xf1, 0x35, 0x34, 0x32, 0x1f, 0xdb,
	0xdc, 0xde, 0xf1, 0x35, 0x34, 0x32, 0x1f, 0xdb, 0xdc, 0xde, 0xf1, 0x35, 0x00, 0xc4, 0x01, 0x1f,
	0x06, 0x00, 0x00, 0x32, 0x1f, 0xdb, 0xcd, 0xde, 0xf1, 0x35, 0x43, 0x32, 0x1f, 0xdb, 0xcd, 0xde,
	0xf1, 0x35, 0x43, 0x32, 0x1f, 0xdb, 0xcd, 0xde, 0xf1, 0x35, 0x43, 0x32, 0x1f, 0xdb, 0xcd, 0xde,
	0xf1, 0x35, 0x43, 0x32, 0x1f, 0xdb, 0xcd, 0xde, 0xf1, 0x35, 0x43, 0x32, 0x1f, 0xdb, 0xcd, 0xde,
	0xf1, 0x35, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x1f, 0x1f,
};

/* ffmpeg's samples [1, 5, 9, 100, 300, 577] of it */
static const int ima_at[] = { 1, 5, 9, 100, 300, 577 };
static const int ima_want[] = { 13, 536, -1554, 4238, -3936, 8 };
static const int ima_n = 605;
/* ffmpeg's samples [1, 5, 9, 100, 300, 577] of it */
static const int ms_at[] = { 1, 5, 9, 100, 300, 577 };
static const int ms_want[] = { 1567, 3890, -1630, 4191, -4014, -40 };
static const int ms_n = 580;

static void be16(char *p, unsigned v) { p[0] = (char)(v >> 8); p[1] = (char)v; }
static void be32(char *p, unsigned long v) { be16(p, (unsigned)(v >> 16)); be16(p + 2, (unsigned)(v & 0xffff)); }

/* sound files other than 16-bit WAV: AIFF, .au, 24-bit and float WAV,
   IMA and Microsoft ADPCM (against ffmpeg's decoding) */
static void test_formats(void)
{
	int16_t *pcm;
	int i, n;
	/* AIFF, 16-bit big-endian, 8000 Hz (80-bit: 0x400B FA00...), mono */
	char a[12 + 26 + 16 + 8];
	memset(a, 0, sizeof(a));
	memcpy(a, "FORM", 4); be32(a + 4, sizeof(a) - 8); memcpy(a + 8, "AIFF", 4);
	memcpy(a + 12, "COMM", 4); be32(a + 16, 18);
	be16(a + 20, 1); be32(a + 22, 4); be16(a + 26, 16);
	a[28] = 0x40; a[29] = 0x0b; a[30] = (char)0xfa;
	memcpy(a + 38, "SSND", 4); be32(a + 42, 8 + 8);
	be16(a + 54, 0x1000); be16(a + 56, 0xf000); be16(a + 58, 0x7fff); be16(a + 60, 0x8000);
	CHECK(dmc_audio_decode(a, sizeof(a), &pcm) == 4);
	CHECK(pcm[0] == 4096 && pcm[1] == -4096 && pcm[2] == 32767 && pcm[3] == -32768);
	dmc_free(pcm);
	/* .au, mu-law, 8000 Hz, mono */
	char u[24 + 3];
	memcpy(u, ".snd", 4); be32(u + 4, 24); be32(u + 8, 3); be32(u + 12, 1);
	be32(u + 16, 8000); be32(u + 20, 1);
	u[24] = (char)0xff; u[25] = 0x00; u[26] = (char)0x80;
	CHECK(dmc_audio_decode(u, sizeof(u), &pcm) == 3);
	CHECK(pcm[0] == 0 && pcm[1] == -32124 && pcm[2] == 32124);
	dmc_free(pcm);
	/* WAV, 24-bit, then 32-bit float */
	char w[44 + 6];
	dmc_wav_header(w, 3);
	w[32] = 3; w[34] = 24; w[40] = 6;
	memcpy(w + 44, "\x00\x00\x40\x00\x00\xc0", 6);
	CHECK(dmc_audio_decode(w, sizeof(w), &pcm) == 2);
	CHECK(pcm[0] == 16384 && pcm[1] == -16384);
	dmc_free(pcm);
	char f[44 + 8];
	float h = 0.5f, m = -1.0f;
	dmc_wav_header(f, 4);
	f[20] = 3; f[32] = 4; f[34] = 32; f[40] = 8;
	memcpy(f + 44, &h, 4); memcpy(f + 48, &m, 4);
	CHECK(dmc_audio_decode(f, sizeof(f), &pcm) == 2);
	CHECK(pcm[0] == 16383 && pcm[1] == -32767);
	dmc_free(pcm);
	/* ADPCM: Microsoft's as ffmpeg decodes it, IMA within a few steps
	   (ffmpeg's arithmetic is finer than the IMA's own) */
	n = dmc_audio_decode((const char *)ms_wav, sizeof(ms_wav), &pcm);
	CHECK(n == ms_n);
	for (i = 0; n == ms_n && i < (int)(sizeof(ms_at) / sizeof(ms_at[0])); i++)
		CHECK(pcm[ms_at[i]] == ms_want[i]);
	dmc_free(pcm);
	n = dmc_audio_decode((const char *)ima_wav, sizeof(ima_wav), &pcm);
	CHECK(n == ima_n);
	for (i = 0; n == ima_n && i < (int)(sizeof(ima_at) / sizeof(ima_at[0])); i++)
		CHECK(abs(pcm[ima_at[i]] - ima_want[i]) <= 8);
	dmc_free(pcm);
	CHECK(dmc_audio_decode("FORM\0\0\0\4AIFF", 12, &pcm) == -1 && !pcm);
	CHECK(dmc_audio_decode("ID3\4\0\0\0\0\0\0\0\0", 12, &pcm) == -1);
}

/* what the codec has for the socket, as a string of escaped bytes */
static int out_is(dmc_net *n, const char *want, int wn)
{
	char b[512];
	int k = dmc_net_output(n, b, sizeof(b));
	return k == wn && !memcmp(b, want, (size_t)wn);
}

static int screen_is(dmc_net *n, const char *want, int wn)
{
	char b[512];
	int k = dmc_net_screen(n, b, sizeof(b));
	return k == wn && !memcmp(b, want, (size_t)wn);
}

static void test_net(void)
{
	CHECK(dmc_net_default_port(DMC_NET_TELNET) == 23);
	CHECK(dmc_net_default_port(DMC_NET_RLOGIN) == 513);
	CHECK(dmc_net_default_port(DMC_NET_SSH) == 22);
	CHECK_STR(dmc_net_name(DMC_NET_RLOGIN), "rlogin");

	/* Telnet: the first offers, then the server's */
	dmc_net *n = dmc_net_new(DMC_NET_TELNET, "", "ANSI", 80, 24);
	dmc_net_connected(n);
	CHECK(out_is(n, "\xff\xfb\x18\xff\xfb\x1f\xff\xfd\x03", 9));
	/* DO TTYPE, DO NAWS (our size), WILL ECHO, WILL SGA (asked:
	   not answered again), DO LINEMODE (refused), text, IAC IAC */
	const char *in = "\xff\xfd\x18\xff\xfd\x1f\xff\xfb\x01\xff\xfb\x03"
		"\xff\xfd\x22Hi\xff\xff!";
	dmc_net_feed(n, in, 20);
	CHECK(out_is(n, "\xff\xfa\x1f\x00\x50\x00\x18\xff\xf0"
		     "\xff\xfd\x01\xff\xfc\x22", 15));
	CHECK(screen_is(n, "Hi\xff!", 4));
	/* TERMINAL-TYPE SEND, split across two reads */
	dmc_net_feed(n, "\xff\xfa\x18", 3);
	dmc_net_feed(n, "\x01\xff\xf0", 3);
	CHECK(out_is(n, "\xff\xfa\x18\x00" "ANSI\xff\xf0", 10));
	/* CR NUL is a CR */
	dmc_net_feed(n, "a\r\0b", 4);
	CHECK(screen_is(n, "a\rb", 3));
	/* typed: a lone CR as CR NUL, IAC doubled */
	dmc_net_type(n, "ls\r\xff", 4);
	CHECK(out_is(n, "ls\r\0\xff\xff", 6));
	dmc_net_type(n, "\r\n", 2);
	CHECK(out_is(n, "\r\n", 2));
	/* resized: NAWS, an 0xff in it doubled */
	dmc_net_resize(n, 255, 50);
	CHECK(out_is(n, "\xff\xfa\x1f\x00\xff\xff\x00\x32\xff\xf0", 10));
	dmc_net_resize(n, 255, 50);
	CHECK(out_is(n, "", 0));
	/* WONT ECHO: DONT; and again: nothing */
	dmc_net_feed(n, "\xff\xfc\x01\xff\xfc\x01", 6);
	CHECK(out_is(n, "\xff\xfe\x01", 3));
	dmc_net_free(n);

	/* rlogin */
	n = dmc_net_new(DMC_NET_RLOGIN, "alice", "xterm", 80, 24);
	dmc_net_connected(n);
	CHECK(out_is(n, "\0alice\0alice\0xterm/38400\0", 25));
	/* the server's NUL, then its text */
	dmc_net_feed(n, "\0Password:", 10);
	CHECK(screen_is(n, "Password:", 9));
	dmc_net_feed(n, "\0", 1);
	CHECK(screen_is(n, "\0", 1));
	/* no size until it is asked for */
	dmc_net_resize(n, 100, 30);
	CHECK(out_is(n, "", 0));
	dmc_net_urgent(n, 0x80);
	CHECK(out_is(n, "\xff\xffss\x00\x1e\x00\x64\x03\x20\x01\xe0", 12));
	dmc_net_resize(n, 80, 24);
	CHECK(out_is(n, "\xff\xffss\x00\x18\x00\x50\x02\x80\x01\x80", 12));
	/* typed as it is */
	dmc_net_type(n, "\r\xff", 2);
	CHECK(out_is(n, "\r\xff", 2));
	/* flush */
	dmc_net_feed(n, "old", 3);
	dmc_net_urgent(n, 0x02);
	CHECK(screen_is(n, "", 0));
	dmc_net_free(n);
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : ".";
	CHECK(dmc_version() == DMC_VERSION);
	test_command();
	test_call();
	test_unsolicited();
	test_typed();
	test_model(dir);
	test_diag(dir);
	test_vt();
	test_vt_profiles();
	test_vt_noise();
	test_daemon();
	test_voice();
	test_voicemail();
	test_wav();
	test_formats();
	test_net();
	printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
