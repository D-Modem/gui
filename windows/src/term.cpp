/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * term.cpp - the Terminal page: the modem's screen (a terminal control,
 *            termview.cpp), with buttons for its full-screen and
 *            command-line setup, the kind of terminal it is (a BBS
 *            reached by modem may want ANSI-BBS, a Commodore's PETSCII,
 *            Viewdata ...), kept in dmodem.ini, and the file transfers
 *            on its line (xfer.cpp).
 */
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "resource.h"

static HWND page, term, type_box;

static void ini(char *out, int n)
{
	char dir[MAX_PATH];
	app_data_dir(dir, sizeof(dir));
	_snprintf(out, n, "%s\\dmodem.ini", dir);
	out[n - 1] = '\0';
}

static void line_write(const char *data, int n)
{
	link_send(data, n);
}

static bool line_open()
{
	return link_open();
}

/* the modem's line, for the file transfers */
XferLine term_line = { "the modem", &term, line_write, link_room, line_open };

static void typed(const char *data, int n, void *ctx)
{
	(void)ctx;
	/* (nothing typed gets into a transfer's packets) */
	if (xfer_busy(&term_line))
		return;
	link_type(data, n);
}

void term_feed(const char *data, int n)
{
	if (term && !xfer_take(&term_line, data, n)) {
		capture_feed(&term_line, data, n);
		termview_feed(term, data, n);
	}
}

void term_state()
{
	if (!page)
		return;
	bool idle = dmc_state(app.s) == DMC_IDLE;
	EnableWindow(GetDlgItem(page, IDC_SETUP_CF), idle);
	EnableWindow(GetDlgItem(page, IDC_SETUP_CT), idle);
}

static void typed_command(const char *cmd)
{
	if (dmc_state(app.s) != DMC_IDLE)
		return;
	link_type(cmd, (int)strlen(cmd));
	SetFocus(term);
}

static void type_changed()
{
	const char *id = term_types_chosen(type_box);
	termview_set_profile(term, id);
	char path[MAX_PATH];
	ini(path, sizeof(path));
	WritePrivateProfileString("terminal", "type", id, path);
}

static void term_command(HWND p, int id, int code)
{
	(void)p;
	switch (id) {
	case IDC_SETUP_CF:
		typed_command("AT!CF\r");
		break;
	case IDC_SETUP_CT:
		typed_command("AT!CT\r");
		break;
	case IDC_CLEAR:
		termview_reset(term);
		SetFocus(term);
		break;
	case IDC_TERM_SEND:
		xfer_send_dialog(&term_line, -1);
		SetFocus(term);
		break;
	case IDC_TERM_RECV:
		xfer_receive_dialog(&term_line);
		SetFocus(term);
		break;
	case IDC_TERM_TYPE:
		if (code == CBN_SELCHANGE)
			type_changed();
		break;
	}
}

HWND term_page_new(HWND parent)
{
	page = page_new(parent, term_command);
	make_child("BUTTON", "&Full-Screen Setup", WS_TABSTOP, page, IDC_SETUP_CF);
	make_child("BUTTON", "&Command-Line Setup", WS_TABSTOP, page,
		   IDC_SETUP_CT);
	make_child("STATIC", "&Type:", SS_RIGHT, page, IDC_TERM_TYPELABEL);
	type_box = make_child("COMBOBOX", "", WS_TABSTOP | CBS_DROPDOWNLIST |
			      WS_VSCROLL, page, IDC_TERM_TYPE);
	make_child("BUTTON", "&Send...", WS_TABSTOP, page, IDC_TERM_SEND);
	make_child("BUTTON", "&Receive...", WS_TABSTOP, page, IDC_TERM_RECV);
	make_child("BUTTON", "C&lear", WS_TABSTOP, page, IDC_CLEAR);
	term = termview_new(page, IDC_TERM, typed, NULL);
	termview_set_printer(term, capture_printed, &term_line);
	/* the modem's configurator wants Backspace as BS */
	termview_set_backspace(term, 0x08);
	char path[MAX_PATH], v[64];
	ini(path, sizeof(path));
	GetPrivateProfileString("terminal", "type", "xterm", v, sizeof(v), path);
	term_types_fill(type_box, v);
	termview_set_profile(term, term_types_chosen(type_box));
	return page;
}

void term_page_layout(HWND p, int w, int h)
{
	int m = 8, bh = 24, xw = 72, cw = 60;
	MoveWindow(GetDlgItem(p, IDC_SETUP_CF), m, m, 140, bh, TRUE);
	MoveWindow(GetDlgItem(p, IDC_SETUP_CT), m + 148, m, 150, bh, TRUE);
	int tx = m + 148 + 158;
	/* the transfers' buttons and Clear at the right */
	int right = w - m - cw - 2 * (xw + m);
	int tw = right - tx - 44 - 2 * m;
	if (tw > 220)
		tw = 220;
	if (tw < 80)
		tw = 80;
	MoveWindow(GetDlgItem(p, IDC_TERM_TYPELABEL), tx, m + 4, 40, bh, TRUE);
	MoveWindow(type_box, tx + 44, m, tw, 300, TRUE);
	MoveWindow(GetDlgItem(p, IDC_TERM_SEND), right, m, xw, bh, TRUE);
	MoveWindow(GetDlgItem(p, IDC_TERM_RECV), right + xw + m, m, xw, bh, TRUE);
	MoveWindow(GetDlgItem(p, IDC_CLEAR), w - m - cw, m, cw, bh, TRUE);
	MoveWindow(term, m, 2 * m + bh, w - 2 * m, h - 3 * m - bh, TRUE);
}
