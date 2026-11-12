/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * vt_keys.c - keys as the octets a VT220 or xterm sends: the cursor keys
 * in normal or application mode, xterm's modifier parameters (ESC[1;5A
 * for Ctrl+Up), the DEC keypad, a VT52's, Ctrl and Alt on characters.
 * A port of the app's KeyEncoder.
 */
#include <stdio.h>
#include <string.h>

#include "vtint.h"

static const char *enter_bytes(const dmc_vt *t)
{
	switch (t->enter_mode) {
	case 1: return "\r\n";
	case 2: return "\n";
	}
	return "\r";
}

/* a keypad key's (NULL: not one): PF1-PF4 as SS3 P-S (a VT52's ESC
   P-S); the others their characters in numeric mode, SS3 and a letter in
   application mode (a VT52's ESC ? and the letter) */
static int keypad(const dmc_vt *t, int k, dmc_buf *out)
{
	int app = t->app_keypad, vt52 = t->vt52_keys, letter;
	char s[8];
	const char *numeric;
	char digit[2];
	if (k >= DMC_KEY_PF1 && k <= DMC_KEY_PF4) {
		snprintf(s, sizeof(s), vt52 ? "\x1b%c" : "\x1bO%c",
			 'P' + (k - DMC_KEY_PF1));
		buf_adds(out, s);
		return 1;
	}
	if (k >= DMC_KEY_KP_0 && k <= DMC_KEY_KP_9) {
		digit[0] = (char)('0' + (k - DMC_KEY_KP_0));
		digit[1] = 0;
		numeric = digit;
		letter = 'p' + (k - DMC_KEY_KP_0);
	} else if (k == DMC_KEY_KP_MINUS) {
		numeric = "-";
		letter = 'm';
	} else if (k == DMC_KEY_KP_COMMA) {
		numeric = ",";
		letter = 'l';
	} else if (k == DMC_KEY_KP_PERIOD) {
		numeric = ".";
		letter = 'n';
	} else if (k == DMC_KEY_KP_ENTER) {
		numeric = enter_bytes(t);
		letter = 'M';
	} else {
		return 0;
	}
	if (!app) {
		buf_adds(out, numeric);
		return 1;
	}
	snprintf(s, sizeof(s), vt52 ? "\x1b?%c" : "\x1bO%c", letter);
	buf_adds(out, s);
	return 1;
}

static const char *vt52_key(int k)
{
	switch (k) {
	case DMC_KEY_UP: return "\x1b" "A";
	case DMC_KEY_DOWN: return "\x1b" "B";
	case DMC_KEY_RIGHT: return "\x1b" "C";
	case DMC_KEY_LEFT: return "\x1b" "D";
	case DMC_KEY_F1: return "\x1bP";
	case DMC_KEY_F2: return "\x1bQ";
	case DMC_KEY_F3: return "\x1bR";
	case DMC_KEY_F4: return "\x1bS";
	}
	return NULL;
}

/* a CSI n ~ key's number, 0 for none */
static int tilde_number(int k)
{
	switch (k) {
	case DMC_KEY_INSERT: return 2;
	case DMC_KEY_DELETE: return 3;
	case DMC_KEY_PGUP: return 5;
	case DMC_KEY_PGDN: return 6;
	case DMC_KEY_F5: return 15;
	case DMC_KEY_F6: return 17;
	case DMC_KEY_F7: return 18;
	case DMC_KEY_F8: return 19;
	case DMC_KEY_F9: return 20;
	case DMC_KEY_F10: return 21;
	case DMC_KEY_F11: return 23;
	case DMC_KEY_F12: return 24;
	case DMC_KEY_F13: return 25;
	case DMC_KEY_F14: return 26;
	case DMC_KEY_F15: return 28;
	case DMC_KEY_F16: return 29;
	case DMC_KEY_F17: return 31;
	case DMC_KEY_F18: return 32;
	case DMC_KEY_F19: return 33;
	case DMC_KEY_F20: return 34;
	case DMC_KEY_FIND: return 1;
	case DMC_KEY_SELECT: return 4;
	}
	return 0;
}

void keys_key(const dmc_vt *t, int k, int mods, dmc_buf *out)
{
	int shift = mods & M_SHIFT, alt = mods & M_ALT, ctrl = mods & M_CTRL;
	int m = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0), n;
	const char *s;
	char b[24];
	int cursor = 0, ss3 = 0;
	if (keypad(t, k, out))
		return;
	if (t->vt52_keys && (s = vt52_key(k)) != NULL) {
		buf_adds(out, s);
		return;
	}
	switch (k) {
	case DMC_KEY_UP: cursor = 'A'; break;
	case DMC_KEY_DOWN: cursor = 'B'; break;
	case DMC_KEY_RIGHT: cursor = 'C'; break;
	case DMC_KEY_LEFT: cursor = 'D'; break;
	case DMC_KEY_HOME: cursor = 'H'; break;
	case DMC_KEY_END: cursor = 'F'; break;
	case DMC_KEY_F1: ss3 = 'P'; break;
	case DMC_KEY_F2: ss3 = 'Q'; break;
	case DMC_KEY_F3: ss3 = 'R'; break;
	case DMC_KEY_F4: ss3 = 'S'; break;
	}
	if (cursor) {
		if (m > 1)
			snprintf(b, sizeof(b), "\x1b[1;%d%c", m, cursor);
		else if (t->app_cursor)
			snprintf(b, sizeof(b), "\x1bO%c", cursor);
		else
			snprintf(b, sizeof(b), "\x1b[%c", cursor);
		buf_adds(out, b);
		return;
	}
	if (ss3) {
		if (m > 1)
			snprintf(b, sizeof(b), "\x1b[1;%d%c", m, ss3);
		else
			snprintf(b, sizeof(b), "\x1bO%c", ss3);
		buf_adds(out, b);
		return;
	}
	n = tilde_number(k);
	if (n) {
		if (m > 1)
			snprintf(b, sizeof(b), "\x1b[%d;%d~", n, m);
		else
			snprintf(b, sizeof(b), "\x1b[%d~", n);
		buf_adds(out, b);
		return;
	}
	switch (k) {
	case DMC_KEY_TAB:
		if (shift) {
			buf_adds(out, "\x1b[Z");
			return;
		}
		if (alt)
			buf_addc(out, 0x1b);
		buf_addc(out, '\t');
		return;
	case DMC_KEY_BACKTAB:
		buf_adds(out, "\x1b[Z");
		return;
	case DMC_KEY_ENTER:
		if (alt)
			buf_addc(out, 0x1b);
		buf_adds(out, enter_bytes(t));
		return;
	case DMC_KEY_BACKSPACE:
		if (alt)
			buf_addc(out, 0x1b);
		buf_addc(out, (char)(ctrl ? 0x08 : t->backspace_code));
		return;
	case DMC_KEY_ESCAPE:
		if (alt)
			buf_addc(out, 0x1b);
		buf_addc(out, 0x1b);
		return;
	}
}

void keys_char(uint32_t cp, int mods, dmc_buf *out)
{
	uint32_t c = cp;
	if (mods & M_CTRL) {
		if (c >= 'a' && c <= 'z')
			c = c - 'a' + 1;
		else if (c >= '@' && c <= '_')
			c = c - '@';
		else if (c == ' ' || c == '2')
			c = 0;
		else if (c == '3')
			c = 0x1b;
		else if (c == '4')
			c = 0x1c;
		else if (c == '5')
			c = 0x1d;
		else if (c == '6')
			c = 0x1e;
		else if (c == '7' || c == '/')
			c = 0x1f;
		else if (c == '8' || c == '?')
			c = 0x7f;
	}
	if (mods & M_ALT)
		buf_addc(out, 0x1b);
	utf8_put(out, c);
}
