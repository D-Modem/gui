/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * settings.cpp - the Settings page: the modem's settings (AT!M), its
 *                groups on the left and their settings on the right;
 *                Change (or a double click) opens a dialog for the
 *                value, which the modem takes at once
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "resource.h"

static HWND page;
static dmc_model *model;
static int group = -1;
static int gen;                 /* a new model: older answers ignored */
static char init_string[512];

/* the settings of the group shown, by row */
static int rows[256], n_rows;

static void set_ansi(HWND w, const char *utf8)
{
	char a[1024];
	to_ansi(utf8, a, sizeof(a));
	SetWindowText(w, a);
}

/* a setting's value as people read it */
static void shown(int i, const char *v, char *out, int n)
{
	int c = dmc_setting_kind(model, i) == DMC_PICK ?
		dmc_setting_choice_of(model, i, v) : -1;
	to_ansi(c >= 0 ? dmc_choice_label(model, i, c) : v, out, n);
}

static void fill_settings()
{
	HWND lv = GetDlgItem(page, IDC_SETTINGS);
	SendMessage(lv, WM_SETREDRAW, FALSE, 0);
	ListView_DeleteAllItems(lv);
	n_rows = 0;
	const char *g = dmc_model_group(model, group);
	for (int i = 0; model && i < dmc_model_count(model) && n_rows < 256; i++) {
		if (strcmp(dmc_setting_group(model, i), g))
			continue;
		char label[160], value[160], def[160];
		to_ansi(dmc_setting_label(model, i), label, sizeof(label));
		shown(i, dmc_setting_value(model, i), value, sizeof(value));
		shown(i, dmc_setting_default(model, i), def, sizeof(def));
		/* a changed one marked, as the configurators mark them */
		if (strcmp(dmc_setting_value(model, i),
			   dmc_setting_default(model, i))) {
			char t[170];
			_snprintf(t, sizeof(t), "* %s", label);
			t[sizeof(t) - 1] = '\0';
			strcpy(label, t);
		}
		LVITEM it;
		memset(&it, 0, sizeof(it));
		it.mask = LVIF_TEXT;
		it.iItem = n_rows;
		it.pszText = label;
		ListView_InsertItem(lv, &it);
		ListView_SetItemText(lv, n_rows, 1, value);
		ListView_SetItemText(lv, n_rows, 2, def);
		rows[n_rows++] = i;
	}
	SendMessage(lv, WM_SETREDRAW, TRUE, 0);
	InvalidateRect(lv, NULL, TRUE);
	SetDlgItemText(page, IDC_SETTING_HELP, "");
}

static void fill_groups()
{
	HWND lb = GetDlgItem(page, IDC_GROUPS);
	SendMessage(lb, LB_RESETCONTENT, 0, 0);
	for (int g = 0; model && g < dmc_model_groups(model); g++) {
		char a[128];
		to_ansi(dmc_model_group(model, g), a, sizeof(a));
		SendMessage(lb, LB_ADDSTRING, 0, (LPARAM)a);
	}
	if (group < 0 || group >= dmc_model_groups(model))
		group = 0;
	SendMessage(lb, LB_SETCURSEL, group, 0);
	fill_settings();
}

void settings_clear()
{
	dmc_model_free(model);
	model = NULL;
	gen++;
	if (!page)
		return;
	SendDlgItemMessage(page, IDC_GROUPS, LB_RESETCONTENT, 0, 0);
	ListView_DeleteAllItems(GetDlgItem(page, IDC_SETTINGS));
	SetDlgItemText(page, IDC_SETTING_HELP, "Start the modem to see its settings.");
	SetDlgItemText(page, IDC_INIT, "");
}

static void got_init(const dmc_event *ev, void *data)
{
	(void)data;
	if (!ev->ok)
		return;
	/* AT!R's lines as one line of commands */
	init_string[0] = '\0';
	const char *p = ev->text;
	while (*p) {
		const char *e = strchr(p, '\n');
		int n = e ? (int)(e - p) : (int)strlen(p);
		char line[256];
		if (n > 255)
			n = 255;
		memcpy(line, p, n);
		line[n] = '\0';
		if (n > 2 && _stricmp(line, "AT")) {
			const char *cmds = init_string[0] ? line + 2 : line;
			if (strlen(init_string) + strlen(cmds) < sizeof(init_string))
				strcat(init_string, cmds);
		}
		p += e ? n + 1 : n;
	}
	SetDlgItemText(page, IDC_INIT, init_string[0] ? init_string :
		       "AT (all at their defaults)");
}

static void got_model(const dmc_event *ev, void *data)
{
	(void)data;
	dmc_model *m = ev->ok ? dmc_model_parse(ev->text) : NULL;
	if (!m) {
		SetDlgItemText(page, IDC_SETTING_HELP, ev->ok ?
			       "The modem's settings list was not readable." :
			       "This modem does not list its settings (AT!M): "
			       "update D-Modem, or use the Terminal's setup "
			       "screen.");
		return;
	}
	dmc_model_free(model);
	model = m;
	gen++;
	fill_groups();
	link_command("AT!R", DMC_QUIET, got_init, NULL);
	settings_state();
}

void settings_load()
{
	if (dmc_state(app.s) == DMC_CLOSED)
		return;
	link_command("AT!M", DMC_QUIET, got_model, NULL);
}

void settings_state()
{
	if (!page)
		return;
	pathview_update();
	bool idle = dmc_state(app.s) == DMC_IDLE && model;
	EnableWindow(GetDlgItem(page, IDC_CHANGE), idle);
	EnableWindow(GetDlgItem(page, IDC_DEFAULTS), idle);
	EnableWindow(GetDlgItem(page, IDC_RELOAD),
		     dmc_state(app.s) != DMC_CLOSED);
}

/*
 *    the Change dialog
 */

struct Change {
	int gen, i;
	char value[256];
};

static Change editing;

static void applied(const dmc_event *ev, void *data)
{
	Change *c = (Change *)data;
	if (model && c->gen == gen) {
		if (ev->ok) {
			dmc_setting_set(model, c->i, c->value);
			/* the country sets others' defaults: all again */
			if (!strcmp(dmc_setting_key(model, c->i), "country"))
				settings_load();
			else
				fill_settings();
			link_command("AT!R", DMC_QUIET, got_init, NULL);
		} else {
			char m[200], l[160];
			to_ansi(dmc_setting_label(model, c->i), l, sizeof(l));
			_snprintf(m, sizeof(m), "The modem refused %s", l);
			m[sizeof(m) - 1] = '\0';
			app_message(m);
		}
	}
	free(c);
}

static BOOL CALLBACK change_proc(HWND d, UINT m, WPARAM wp, LPARAM lp)
{
	(void)lp;
	int i = editing.i;
	switch (m) {
	case WM_INITDIALOG: {
		set_ansi(GetDlgItem(d, IDC_S_LABEL), dmc_setting_label(model, i));
		set_ansi(GetDlgItem(d, IDC_S_HELP), dmc_setting_help(model, i));
		char def[200], t[220];
		shown(i, dmc_setting_default(model, i), def, sizeof(def));
		_snprintf(t, sizeof(t), "Default: %s", def);
		t[sizeof(t) - 1] = '\0';
		SetDlgItemText(d, IDC_S_DEFAULT, t);
		int kind = dmc_setting_kind(model, i);
		HWND list = GetDlgItem(d, IDC_S_LIST), edit = GetDlgItem(d, IDC_S_EDIT);
		ShowWindow(list, kind == DMC_PICK ? SW_SHOW : SW_HIDE);
		ShowWindow(edit, kind == DMC_PICK ? SW_HIDE : SW_SHOW);
		if (kind == DMC_PICK) {
			for (int c = 0; c < dmc_setting_choices(model, i); c++) {
				char a[160];
				to_ansi(dmc_choice_label(model, i, c), a, sizeof(a));
				SendMessage(list, CB_ADDSTRING, 0, (LPARAM)a);
			}
			SendMessage(list, CB_SETCURSEL,
				    dmc_setting_choice_of(model, i,
							  dmc_setting_value(model, i)), 0);
		} else {
			SetWindowText(edit, dmc_setting_value(model, i));
			if (kind == DMC_NUMBER) {
				_snprintf(t, sizeof(t), "From %ld to %ld",
					  dmc_setting_lo(model, i),
					  dmc_setting_hi(model, i));
				t[sizeof(t) - 1] = '\0';
				SetDlgItemText(d, IDC_S_RANGE, t);
			} else {
				SetDlgItemText(d, IDC_S_RANGE,
					       "Numbers apart by commas");
			}
		}
		return TRUE;
	}
	case WM_COMMAND:
		if (LOWORD(wp) == IDOK) {
			const char *v;
			char text[256];
			if (dmc_setting_kind(model, i) == DMC_PICK) {
				int c = (int)SendDlgItemMessage(d, IDC_S_LIST,
								CB_GETCURSEL, 0, 0);
				v = dmc_choice_value(model, i, c);
			} else {
				GetDlgItemText(d, IDC_S_EDIT, text, sizeof(text));
				v = text;
			}
			const char *why = dmc_setting_check(model, i, v);
			if (*why) {
				MessageBox(d, why, "D-Modem", MB_OK | MB_ICONWARNING);
				return TRUE;
			}
			strncpy(editing.value, v, sizeof(editing.value) - 1);
			editing.value[sizeof(editing.value) - 1] = '\0';
			EndDialog(d, IDOK);
			return TRUE;
		}
		if (LOWORD(wp) == IDCANCEL) {
			EndDialog(d, IDCANCEL);
			return TRUE;
		}
		break;
	}
	return FALSE;
}

static void change_selected()
{
	if (!model || dmc_state(app.s) != DMC_IDLE)
		return;
	int r = ListView_GetNextItem(GetDlgItem(page, IDC_SETTINGS), -1,
				     LVNI_SELECTED);
	if (r < 0 || r >= n_rows)
		return;
	memset(&editing, 0, sizeof(editing));
	editing.gen = gen;
	editing.i = rows[r];
	if (DialogBox(app.inst, MAKEINTRESOURCE(IDD_SETTING), app.main,
		      (DLGPROC)change_proc) != IDOK)
		return;
	if (!strcmp(editing.value, dmc_setting_value(model, editing.i)))
		return;
	Change *c = (Change *)malloc(sizeof(Change));
	*c = editing;
	if (link_command(dmc_setting_command(model, c->i, c->value), 0, applied,
			 c) < 0) {
		app_message("The modem is busy");
		free(c);
	}
}

static void reset_done(const dmc_event *ev, void *data)
{
	(void)data;
	if (!ev->ok)
		app_message("The modem refused ATZ");
	settings_load();
}

static void settings_command(HWND p, int id, int code)
{
	(void)p;
	switch (id) {
	case IDC_GROUPS:
		if (code == LBN_SELCHANGE) {
			group = (int)SendDlgItemMessage(page, IDC_GROUPS,
							LB_GETCURSEL, 0, 0);
			fill_settings();
		}
		break;
	case IDC_CHANGE:
		change_selected();
		break;
	case IDC_RELOAD:
		settings_load();
		break;
	case IDC_DEFAULTS:
		if (MessageBox(app.main, "Put every setting back to its default "
			       "(ATZ)?", "D-Modem", MB_OKCANCEL |
			       MB_ICONQUESTION) == IDOK)
			link_command("ATZ", 0, reset_done, NULL);
		break;
	case IDC_KEEP:
		strncpy(app.cfg.init, init_string, sizeof(app.cfg.init) - 1);
		app.cfg.init[sizeof(app.cfg.init) - 1] = '\0';
		config_save(&app.cfg);
		app_message(init_string[0] ? "The init string is sent each time "
			    "the modem opens" : "Nothing to keep: every setting "
			    "is at its default");
		break;
	}
}

/* the list's notifications come through the main window */
static WNDPROC list_old;

static LRESULT CALLBACK list_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
	if (m == WM_KEYDOWN && wp == VK_RETURN) {
		change_selected();
		return 0;
	}
	if (m == WM_LBUTTONDBLCLK) {
		CallWindowProc(list_old, w, WM_LBUTTONDOWN, wp, lp);
		change_selected();
		return 0;
	}
	if (m == WM_GETDLGCODE && lp) {
		MSG *msg = (MSG *)lp;
		if (msg->message == WM_KEYDOWN && msg->wParam == VK_RETURN)
			return DLGC_WANTMESSAGE;
	}
	LRESULT r = CallWindowProc(list_old, w, m, wp, lp);
	/* (the selection's help) */
	if (m == WM_LBUTTONDOWN || m == WM_KEYUP) {
		int row = ListView_GetNextItem(w, -1, LVNI_SELECTED);
		if (model && row >= 0 && row < n_rows)
			set_ansi(GetDlgItem(page, IDC_SETTING_HELP),
				 dmc_setting_help(model, rows[row]));
	}
	return r;
}

HWND settings_page_new(HWND parent)
{
	page = page_new(parent, settings_command);
	/* how a call is made, step by step: as set up when idle, as it is
	   in a call */
	pathview_new(page);
	make_child("LISTBOX", "", WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY |
		   LBS_NOINTEGRALHEIGHT, page, IDC_GROUPS, WS_EX_CLIENTEDGE);
	HWND lv = make_child(WC_LISTVIEW, "", WS_TABSTOP | LVS_REPORT |
			     LVS_SINGLESEL | LVS_SHOWSELALWAYS |
			     LVS_NOSORTHEADER, page, IDC_SETTINGS,
			     WS_EX_CLIENTEDGE);
	ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT);
	LVCOLUMN c;
	memset(&c, 0, sizeof(c));
	c.mask = LVCF_TEXT | LVCF_WIDTH;
	c.pszText = (char *)"Setting";
	c.cx = 200;
	ListView_InsertColumn(lv, 0, &c);
	c.pszText = (char *)"Value";
	c.cx = 140;
	ListView_InsertColumn(lv, 1, &c);
	c.pszText = (char *)"Default";
	c.cx = 110;
	ListView_InsertColumn(lv, 2, &c);
	list_old = (WNDPROC)SetWindowLongPtr(lv, GWLP_WNDPROC,
					     (LONG_PTR)list_proc);
	make_child("STATIC", "", SS_LEFT | SS_NOPREFIX, page, IDC_SETTING_HELP);
	make_child("BUTTON", "&Change...", WS_TABSTOP, page, IDC_CHANGE);
	make_child("BUTTON", "&Reload", WS_TABSTOP, page, IDC_RELOAD);
	make_child("BUTTON", "&Defaults", WS_TABSTOP, page, IDC_DEFAULTS);
	make_child("EDIT", "", WS_TABSTOP | ES_READONLY | ES_AUTOHSCROLL, page,
		   IDC_INIT, WS_EX_CLIENTEDGE);
	make_child("BUTTON", "&Keep", WS_TABSTOP, page, IDC_KEEP);
	settings_clear();
	settings_state();
	return page;
}

void settings_page_layout(HWND p, int w, int h)
{
	int m = 8, bh = 24, gw = 130, bw = 90;
	int ph = pathview_height();
	MoveWindow(FindWindowEx(p, NULL, "DModemPath", NULL), m, m,
		   w - 2 * m, ph, TRUE);
	int top = m + ph + m;
	int listh = h - top - 3 * m - 2 * bh - 36;
	MoveWindow(GetDlgItem(p, IDC_GROUPS), m, top, gw, listh, TRUE);
	MoveWindow(GetDlgItem(p, IDC_SETTINGS), 2 * m + gw, top, w - 3 * m - gw,
		   listh, TRUE);
	int y = top + listh + m;
	MoveWindow(GetDlgItem(p, IDC_SETTING_HELP), m, y, w - 2 * m, 32, TRUE);
	y += 36;
	MoveWindow(GetDlgItem(p, IDC_CHANGE), m, y, bw, bh, TRUE);
	MoveWindow(GetDlgItem(p, IDC_RELOAD), 2 * m + bw, y, bw, bh, TRUE);
	MoveWindow(GetDlgItem(p, IDC_DEFAULTS), 3 * m + 2 * bw, y, bw, bh, TRUE);
	y += bh + m;
	MoveWindow(GetDlgItem(p, IDC_INIT), m, y, w - 3 * m - bw, bh, TRUE);
	MoveWindow(GetDlgItem(p, IDC_KEEP), w - m - bw, y, bw, bh, TRUE);
}
