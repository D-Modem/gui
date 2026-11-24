/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * termset.cpp - Terminal Settings, as HyperTerminal has them, for each
 *               terminal (the modem's, kept in [terminal] of dmodem.ini;
 *               the Remote page's, in [remote_terminal]): what Backspace
 *               sends, the function, arrow and Ctrl keys as terminal or
 *               Windows keys, the backscroll buffer's lines, the font,
 *               the cursor, the Telnet terminal ID (Remote), and the
 *               exit upon disconnecting (the modem).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "resource.h"

static void ini(char *out, int n)
{
	char dir[MAX_PATH];
	app_data_dir(dir, sizeof(dir));
	_snprintf(out, n, "%s\\dmodem.ini", dir);
	out[n - 1] = '\0';
}

/* the section of a terminal: the Remote page's, or the modem's */
static const char *section(bool remote)
{
	return remote ? "remote_terminal" : "terminal";
}

static int get(bool remote, const char *key, int def)
{
	char path[MAX_PATH];
	ini(path, sizeof(path));
	return (int)GetPrivateProfileInt(section(remote), key, def, path);
}

static void get_str(bool remote, const char *key, char *out, int n)
{
	char path[MAX_PATH];
	ini(path, sizeof(path));
	GetPrivateProfileString(section(remote), key, "", out, n, path);
}

static void put(bool remote, const char *key, int v)
{
	char path[MAX_PATH], s[16];
	ini(path, sizeof(path));
	_snprintf(s, sizeof(s), "%d", v);
	s[sizeof(s) - 1] = '\0';
	WritePrivateProfileString(section(remote), key, s, path);
}

static void put_str(bool remote, const char *key, const char *v)
{
	char path[MAX_PATH];
	ini(path, sizeof(path));
	WritePrivateProfileString(section(remote), key, v, path);
}

struct TermSet {
	bool remote;
	TermOptions o;
	char telnet_id[40];
	bool exit_on_disconnect;
};

static void load(bool remote, TermSet *s)
{
	memset(s, 0, sizeof(*s));
	s->remote = remote;
	/* (the modem's configurator wants Ctrl+H; a remote shell, Del) */
	s->o.backspace = get(remote, "backspace", remote ? 1 : 0);
	if (s->o.backspace < 0 || s->o.backspace > 2)
		s->o.backspace = remote ? 1 : 0;
	s->o.windows_keys = get(remote, "windows_keys", 0) != 0;
	s->o.scrollback = get(remote, "backscroll", 2000);
	if (s->o.scrollback < 0)
		s->o.scrollback = 0;
	if (s->o.scrollback > 100000)
		s->o.scrollback = 100000;
	get_str(remote, "font", s->o.font, sizeof(s->o.font));
	s->o.font_size = get(remote, "font_size", 10);
	s->o.cursor = get(remote, "cursor", 0) == 1 ? 1 : 0;
	s->o.cursor_blink = get(remote, "cursor_blink", 1) != 0;
	if (remote)
		get_str(remote, "telnet_id", s->telnet_id, sizeof(s->telnet_id));
	else
		s->exit_on_disconnect = get(remote, "exit_on_disconnect", 0) != 0;
}

static void save(const TermSet *s)
{
	bool r = s->remote;
	put(r, "backspace", s->o.backspace);
	put(r, "windows_keys", s->o.windows_keys);
	put(r, "backscroll", s->o.scrollback);
	put_str(r, "font", s->o.font);
	put(r, "font_size", s->o.font_size);
	put(r, "cursor", s->o.cursor);
	put(r, "cursor_blink", s->o.cursor_blink);
	if (r)
		put_str(r, "telnet_id", s->telnet_id);
	else
		put(r, "exit_on_disconnect", s->exit_on_disconnect);
}

void termset_apply(HWND view, bool remote)
{
	if (!view)
		return;
	TermSet s;
	load(remote, &s);
	termview_set_options(view, &s.o);
}

void termset_telnet_id(char *out, int n)
{
	get_str(true, "telnet_id", out, n);
}

bool termset_exit_on_disconnect()
{
	return get(false, "exit_on_disconnect", 0) != 0;
}

static int num(HWND d, int id, int lo, int hi, int def)
{
	BOOL ok;
	UINT v = GetDlgItemInt(d, id, &ok, FALSE);
	if (!ok)
		return def;
	if ((int)v < lo)
		return lo;
	return (int)v > hi ? hi : (int)v;
}

static INT_PTR CALLBACK termset_proc(HWND d, UINT m, WPARAM wp, LPARAM lp)
{
	TermSet *s = (TermSet *)GetWindowLongPtr(d, DWLP_USER);
	switch (m) {
	case WM_INITDIALOG:
		s = (TermSet *)lp;
		SetWindowLongPtr(d, DWLP_USER, (LONG_PTR)s);
		SetWindowText(d, s->remote ? "Terminal Settings: Remote" :
			      "Terminal Settings: Modem");
		CheckRadioButton(d, IDC_T_BSH, IDC_T_BSHSH,
				 IDC_T_BSH + s->o.backspace);
		CheckRadioButton(d, IDC_T_TERMKEYS, IDC_T_WINKEYS,
				 s->o.windows_keys ? IDC_T_WINKEYS : IDC_T_TERMKEYS);
		SetDlgItemInt(d, IDC_T_BACKSCROLL, (UINT)s->o.scrollback, FALSE);
		SetDlgItemText(d, IDC_T_FONT, s->o.font);
		SetDlgItemInt(d, IDC_T_FONTSIZE, (UINT)s->o.font_size, FALSE);
		CheckRadioButton(d, IDC_T_BLOCK, IDC_T_UNDERLINE,
				 s->o.cursor ? IDC_T_UNDERLINE : IDC_T_BLOCK);
		CheckDlgButton(d, IDC_T_BLINK, s->o.cursor_blink ? BST_CHECKED : BST_UNCHECKED);
		SetDlgItemText(d, IDC_T_TELNETID, s->telnet_id);
		EnableWindow(GetDlgItem(d, IDC_T_TELNETID), s->remote);
		EnableWindow(GetDlgItem(d, IDC_T_TELNETLABEL), s->remote);
		CheckDlgButton(d, IDC_T_EXIT, s->exit_on_disconnect ? BST_CHECKED : BST_UNCHECKED);
		EnableWindow(GetDlgItem(d, IDC_T_EXIT), !s->remote);
		return TRUE;
	case WM_COMMAND:
		switch (LOWORD(wp)) {
		case IDOK:
			for (int i = 0; i < 3; i++)
				if (IsDlgButtonChecked(d, IDC_T_BSH + i) == BST_CHECKED)
					s->o.backspace = i;
			s->o.windows_keys = IsDlgButtonChecked(d, IDC_T_WINKEYS) == BST_CHECKED;
			s->o.scrollback = num(d, IDC_T_BACKSCROLL, 0, 100000, 2000);
			GetDlgItemText(d, IDC_T_FONT, s->o.font, sizeof(s->o.font));
			s->o.font_size = num(d, IDC_T_FONTSIZE, 6, 72, 10);
			s->o.cursor = IsDlgButtonChecked(d, IDC_T_UNDERLINE) == BST_CHECKED;
			s->o.cursor_blink = IsDlgButtonChecked(d, IDC_T_BLINK) == BST_CHECKED;
			GetDlgItemText(d, IDC_T_TELNETID, s->telnet_id, sizeof(s->telnet_id));
			s->exit_on_disconnect = IsDlgButtonChecked(d, IDC_T_EXIT) == BST_CHECKED;
			EndDialog(d, IDOK);
			return TRUE;
		case IDCANCEL:
			EndDialog(d, IDCANCEL);
			return TRUE;
		}
		break;
	}
	return FALSE;
}

void termset_dialog(HWND parent, bool remote)
{
	TermSet s;
	load(remote, &s);
	if (DialogBoxParam(app.inst, MAKEINTRESOURCE(IDD_TERMSET), parent,
			   termset_proc, (LPARAM)&s) != IDOK)
		return;
	save(&s);
	termset_apply(remote ? *remote_line.view : *term_line.view, remote);
}
