/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026, D-Modem
 *
 * pages.cpp - the Modem, Call Stats and Log pages
 */
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "resource.h"


static LRESULT CALLBACK page_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
	if (m == WM_COMMAND) {
		CommandFn fn = (CommandFn)GetWindowLongPtr(w, GWLP_USERDATA);
		if (fn)
			fn(w, LOWORD(wp), HIWORD(wp));
		return 0;
	}
	if (m == WM_NOTIFY || m == WM_CTLCOLORSTATIC || m == WM_DRAWITEM)
		return SendMessage(GetParent(w), m, wp, lp);
	return DefWindowProc(w, m, wp, lp);
}

HWND page_new(HWND parent, CommandFn fn)
{
	static bool registered;
	if (!registered) {
		WNDCLASS wc;
		memset(&wc, 0, sizeof(wc));
		wc.lpfnWndProc = page_proc;
		wc.hInstance = app.inst;
		wc.hCursor = LoadCursor(NULL, IDC_ARROW);
		wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
		wc.lpszClassName = "DModemPage";
		RegisterClass(&wc);
		registered = true;
	}
	HWND w = CreateWindowEx(WS_EX_CONTROLPARENT, "DModemPage", "",
				WS_CHILD | WS_CLIPCHILDREN, 0, 0, 10, 10, parent,
				NULL, app.inst, NULL);
	SetWindowLongPtr(w, GWLP_USERDATA, (LONG_PTR)fn);
	return w;
}

static void set_text(HWND page, int id, const char *utf8)
{
	char a[512];
	to_ansi(utf8, a, sizeof(a));
	SetDlgItemText(page, id, a);
}

/*
 *    the Modem page
 */

static HWND modem;

void modem_page_state()
{
	if (!modem)
		return;
	int st = dmc_state(app.s);
	bool running = link_running();
	set_text(modem, IDC_STATE, running && st == DMC_CLOSED ? "Starting" :
		 dmc_state_name(st));
	char d[400];
	if (st == DMC_ONLINE || st == DMC_ONLINE_COMMAND || st == DMC_HANGING_UP)
		_snprintf(d, sizeof(d), "%s", dmc_connect_text(app.s));
	else if (st == DMC_RINGING && dmc_caller_number(app.s)[0])
		_snprintf(d, sizeof(d), "%s %s", dmc_caller_number(app.s),
			  dmc_caller_name(app.s));
	else
		d[0] = '\0';
	d[sizeof(d) - 1] = '\0';
	set_text(modem, IDC_DETAIL, d);
	char sum[400];
	config_summary(&app.cfg, sum, sizeof(sum));
	set_text(modem, IDC_SUMMARY, sum);
	char dev[300];
	_snprintf(dev, sizeof(dev), "Device: %s", link_open() ? app.device :
		  "not connected");
	dev[sizeof(dev) - 1] = '\0';
	set_text(modem, IDC_DEVICE, dev);
	SetDlgItemText(modem, IDC_RUN, running ? "&Stop" : "&Start");
	bool open = st != DMC_CLOSED;
	EnableWindow(GetDlgItem(modem, IDC_NUMBER), open);
	EnableWindow(GetDlgItem(modem, IDC_DIAL), st == DMC_IDLE);
	EnableWindow(GetDlgItem(modem, IDC_ANSWER), st == DMC_RINGING);
	EnableWindow(GetDlgItem(modem, IDC_HANGUP),
		     st == DMC_DIALING || st == DMC_ANSWERING ||
		     st == DMC_ONLINE || st == DMC_ONLINE_COMMAND ||
		     st == DMC_RINGING);
}

static void dialled(const dmc_event *ev, void *data)
{
	(void)data;
	if (!ev->ok)
		app_message(ev->result);
}

static void modem_command(HWND page, int id, int code)
{
	(void)code;
	switch (id) {
	case IDC_DIAL: {
		char n[128], cmd[140];
		GetDlgItemText(page, IDC_NUMBER, n, sizeof(n));
		if (!n[0]) {
			app_message("Enter a number to dial");
			return;
		}
		_snprintf(cmd, sizeof(cmd), "ATD%s", n);
		cmd[sizeof(cmd) - 1] = '\0';
		link_command(cmd, 0, dialled, NULL);
		break;
	}
	case IDC_ANSWER:
		link_command("ATA", 0, dialled, NULL);
		break;
	case IDC_HANGUP:
		dmc_hangup(app.s);
		link_pump();
		break;
	case IDC_RUN:
		SendMessage(app.main, WM_COMMAND, IDM_START, 0);
		break;
	case IDC_LINEBTN:
		SendMessage(app.main, WM_COMMAND, IDM_LINE, 0);
		break;
	}
}

HWND modem_page_new(HWND parent)
{
	modem = page_new(parent, modem_command);
	HWND st = make_child("STATIC", "Not connected", SS_LEFT, modem, IDC_STATE);
	SendMessage(st, WM_SETFONT, (WPARAM)app.big_font, TRUE);
	make_child("STATIC", "", SS_LEFT | SS_NOPREFIX, modem, IDC_DETAIL);
	make_child("BUTTON", "Call", BS_GROUPBOX, modem, IDC_CALLBOX);
	make_child("STATIC", "&Number:", SS_LEFT, modem, IDC_NUMLABEL);
	make_child("EDIT", "", WS_TABSTOP | ES_AUTOHSCROLL, modem, IDC_NUMBER,
		   WS_EX_CLIENTEDGE);
	make_child("BUTTON", "&Dial", WS_TABSTOP | BS_DEFPUSHBUTTON, modem,
		   IDC_DIAL);
	make_child("BUTTON", "&Answer", WS_TABSTOP, modem, IDC_ANSWER);
	make_child("BUTTON", "&Hang Up", WS_TABSTOP, modem, IDC_HANGUP);
	make_child("BUTTON", "Modem", BS_GROUPBOX, modem, IDC_MODEMBOX);
	make_child("STATIC", "", SS_LEFT | SS_NOPREFIX, modem, IDC_SUMMARY);
	make_child("STATIC", "", SS_LEFT | SS_NOPREFIX, modem, IDC_DEVICE);
	make_child("BUTTON", "&Start", WS_TABSTOP, modem, IDC_RUN);
	make_child("BUTTON", "&Line Settings...", WS_TABSTOP, modem,
		   IDC_LINEBTN);
	return modem;
}

static void place(HWND page, int id, int x, int y, int w, int h)
{
	MoveWindow(GetDlgItem(page, id), x, y, w, h, TRUE);
}

void modem_page_layout(HWND page, int w, int h)
{
	(void)h;
	int m = 12, bw = 90, bh = 24, x = m, y = m;
	int inner = w - 2 * m;
	if (inner > 560)
		inner = 560;
	place(page, IDC_STATE, x, y, inner, 40);
	y += 44;
	place(page, IDC_DETAIL, x, y, inner, 20);
	y += 32;
	place(page, IDC_CALLBOX, x, y, inner, 96);
	place(page, IDC_NUMLABEL, x + 12, y + 28, 60, 20);
	place(page, IDC_NUMBER, x + 76, y + 24, inner - 76 - bw - 24, bh);
	place(page, IDC_DIAL, x + inner - bw - 12, y + 24, bw, bh);
	place(page, IDC_ANSWER, x + inner - 2 * bw - 18, y + 60, bw, bh);
	place(page, IDC_HANGUP, x + inner - bw - 12, y + 60, bw, bh);
	y += 108;
	place(page, IDC_MODEMBOX, x, y, inner, 104);
	place(page, IDC_SUMMARY, x + 12, y + 24, inner - bw - 36, 20);
	place(page, IDC_DEVICE, x + 12, y + 48, inner - bw - 36, 20);
	place(page, IDC_RUN, x + inner - bw - 12, y + 22, bw, bh);
	place(page, IDC_LINEBTN, x + inner - 130 - 12, y + 66, 130, bh);
}

/*
 *    the Call Stats page
 */

static HWND stats;

static void got_report(const dmc_event *ev, void *data)
{
	(void)data;
	/* the edit control wants CR LF */
	char a[4096], b[8192];
	to_ansi(ev->ok ? ev->text : "", a, sizeof(a));
	int o = 0;
	for (int i = 0; a[i] && o + 2 < (int)sizeof(b); i++) {
		if (a[i] == '\n')
			b[o++] = '\r';
		b[o++] = a[i];
	}
	b[o] = '\0';
	SetDlgItemText(stats, IDC_REPORT, b);
}

static void got_diag(const dmc_event *ev, void *data)
{
	(void)data;
	HWND lv = GetDlgItem(stats, IDC_STATS);
	ListView_DeleteAllItems(lv);
	dmc_diag *d = dmc_diag_parse(ev->ok ? ev->text : "");
	if (!dmc_diag_has_call(d)) {
		LVITEM it;
		memset(&it, 0, sizeof(it));
		it.mask = LVIF_TEXT;
		it.pszText = (char *)"No call yet";
		ListView_InsertItem(lv, &it);
	}
	for (int i = 0, row = 0; dmc_diag_has_call(d) && i < dmc_diag_count(d);
	     i++) {
		if (dmc_diag_key(d, i) == 0)
			continue;
		char label[128], value[256];
		to_ansi(dmc_diag_label(d, i), label, sizeof(label));
		to_ansi(dmc_diag_value(d, i), value, sizeof(value));
		LVITEM it;
		memset(&it, 0, sizeof(it));
		it.mask = LVIF_TEXT;
		it.iItem = row;
		it.pszText = label;
		ListView_InsertItem(lv, &it);
		ListView_SetItemText(lv, row, 1, value);
		row++;
	}
	dmc_diag_free(d);
}

void stats_refresh()
{
	int st = dmc_state(app.s);
	if (st == DMC_CLOSED || st == DMC_ONLINE)
		return;
	link_command("AT#UD", DMC_QUIET, got_diag, NULL);
	link_command("ATI11", DMC_QUIET, got_report, NULL);
}

static void stats_command(HWND page, int id, int code)
{
	(void)page;
	(void)code;
	if (id == IDC_REFRESH)
		stats_refresh();
}

HWND stats_page_new(HWND parent)
{
	stats = page_new(parent, stats_command);
	HWND lv = make_child(WC_LISTVIEW, "", WS_TABSTOP | LVS_REPORT |
			     LVS_SINGLESEL | LVS_NOSORTHEADER, stats, IDC_STATS,
			     WS_EX_CLIENTEDGE);
	ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT);
	LVCOLUMN c;
	memset(&c, 0, sizeof(c));
	c.mask = LVCF_TEXT | LVCF_WIDTH;
	c.pszText = (char *)"Last call";
	c.cx = 200;
	ListView_InsertColumn(lv, 0, &c);
	c.pszText = (char *)"Value";
	c.cx = 220;
	ListView_InsertColumn(lv, 1, &c);
	HWND r = make_child("EDIT", "", WS_TABSTOP | WS_VSCROLL | ES_MULTILINE |
			    ES_READONLY | ES_AUTOVSCROLL, stats, IDC_REPORT,
			    WS_EX_CLIENTEDGE);
	SendMessage(r, WM_SETFONT, (WPARAM)app.mono_font, TRUE);
	make_child("BUTTON", "&Refresh", WS_TABSTOP, stats, IDC_REFRESH);
	return stats;
}

void stats_page_layout(HWND page, int w, int h)
{
	int m = 8, bh = 24;
	int lvw = (w - 3 * m) / 2;
	place(page, IDC_STATS, m, m, lvw, h - 3 * m - bh);
	place(page, IDC_REPORT, 2 * m + lvw, m, w - 3 * m - lvw, h - 3 * m - bh);
	place(page, IDC_REFRESH, m, h - m - bh, 90, bh);
}

/*
 *    the Log page
 */

static HWND logp;

void log_append(const char *line)
{
	if (!logp)
		return;
	HWND e = GetDlgItem(logp, IDC_LOG);
	char a[1100];
	to_ansi(line, a, sizeof(a) - 2);
	strcat(a, "\r\n");
	/* (Windows 9x's edit controls hold 64K) */
	int n = GetWindowTextLength(e);
	if (n > 48000) {
		SendMessage(e, EM_SETSEL, 0, 16000);
		SendMessage(e, EM_REPLACESEL, FALSE, (LPARAM)"");
		n = GetWindowTextLength(e);
	}
	SendMessage(e, EM_SETSEL, n, n);
	SendMessage(e, EM_REPLACESEL, FALSE, (LPARAM)a);
}

static void log_command(HWND page, int id, int code)
{
	(void)page;
	(void)id;
	(void)code;
}

HWND log_page_new(HWND parent)
{
	logp = page_new(parent, log_command);
	HWND e = make_child("EDIT", "", WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
			    ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL, logp,
			    IDC_LOG, WS_EX_CLIENTEDGE);
	SendMessage(e, WM_SETFONT, (WPARAM)app.mono_font, TRUE);
	SendMessage(e, EM_LIMITTEXT, 60000, 0);
	return logp;
}

void log_page_layout(HWND page, int w, int h)
{
	place(page, IDC_LOG, 8, 8, w - 16, h - 16);
}
