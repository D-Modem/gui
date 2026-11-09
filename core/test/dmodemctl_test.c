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
	/* resized */
	dmc_vt_resize(t, 80, 24);
	CHECK(dmc_vt_cols(t) == 80 && dmc_vt_rows(t) == 24);
	CHECK_STR(dmc_vt_text(t, 3), "01234567890123456789");
	CHECK(dmc_vt_palette(4) == 0x0000AA);
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
	test_daemon();
	printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
