/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * ascii.cpp - ASCII Setup, as HyperTerminal has it, kept in dmodem.ini
 *             ([ascii]): for the data sent, Enter as CR LF, the local
 *             echo, and the line and character delays of a text file
 *             sent (xfer.cpp); for the data received, an LF after each
 *             CR, 7-bit ASCII, and the wrap at the right margin.  The
 *             settings apply to the two terminals (the modem's and the
 *             Remote page's).
 */
#include <stdio.h>
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

static int get(const char *key, int def)
{
	char path[MAX_PATH];
	ini(path, sizeof(path));
	return (int)GetPrivateProfileInt("ascii", key, def, path);
}

static void put(const char *key, int v)
{
	char path[MAX_PATH], s[16];
	ini(path, sizeof(path));
	_snprintf(s, sizeof(s), "%d", v);
	s[sizeof(s) - 1] = '\0';
	WritePrivateProfileString("ascii", key, s, path);
}

void ascii_load(AsciiSetup *a)
{
	a->send_lf = get("send_lf", 0) != 0;
	a->echo = get("echo", 0) != 0;
	a->line_delay = get("line_delay", 0);
	a->char_delay = get("char_delay", 0);
	a->add_lf = get("add_lf", 0) != 0;
	a->seven_bit = get("seven_bit", 0) != 0;
	a->wrap = get("wrap", 1) != 0;
}

static void save(const AsciiSetup *a)
{
	put("send_lf", a->send_lf);
	put("echo", a->echo);
	put("line_delay", a->line_delay);
	put("char_delay", a->char_delay);
	put("add_lf", a->add_lf);
	put("seven_bit", a->seven_bit);
	put("wrap", a->wrap);
}

void ascii_apply(HWND view)
{
	if (!view)
		return;
	AsciiSetup a;
	ascii_load(&a);
	int flags = (a.add_lf ? DMC_VT_ASCII_ADD_LF : 0) |
		    (a.seven_bit ? DMC_VT_ASCII_7BIT : 0) |
		    (a.wrap ? 0 : DMC_VT_ASCII_NO_WRAP);
	termview_set_ascii(view, flags, a.send_lf ? 1 : 0, a.echo);
}

/* a delay box: 0 to 9999 ms */
static int delay_of(HWND d, int id)
{
	BOOL ok;
	UINT v = GetDlgItemInt(d, id, &ok, FALSE);
	if (!ok)
		return 0;
	return v > 9999 ? 9999 : (int)v;
}

static INT_PTR CALLBACK ascii_proc(HWND d, UINT m, WPARAM wp, LPARAM lp)
{
	AsciiSetup *a = (AsciiSetup *)GetWindowLongPtr(d, DWLP_USER);
	switch (m) {
	case WM_INITDIALOG:
		a = (AsciiSetup *)lp;
		SetWindowLongPtr(d, DWLP_USER, (LONG_PTR)a);
		CheckDlgButton(d, IDC_A_SENDLF, a->send_lf ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(d, IDC_A_ECHO, a->echo ? BST_CHECKED : BST_UNCHECKED);
		SetDlgItemInt(d, IDC_A_LINEDELAY, (UINT)a->line_delay, FALSE);
		SetDlgItemInt(d, IDC_A_CHARDELAY, (UINT)a->char_delay, FALSE);
		CheckDlgButton(d, IDC_A_ADDLF, a->add_lf ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(d, IDC_A_7BIT, a->seven_bit ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(d, IDC_A_WRAP, a->wrap ? BST_CHECKED : BST_UNCHECKED);
		return TRUE;
	case WM_COMMAND:
		switch (LOWORD(wp)) {
		case IDOK:
			a->send_lf = IsDlgButtonChecked(d, IDC_A_SENDLF) == BST_CHECKED;
			a->echo = IsDlgButtonChecked(d, IDC_A_ECHO) == BST_CHECKED;
			a->line_delay = delay_of(d, IDC_A_LINEDELAY);
			a->char_delay = delay_of(d, IDC_A_CHARDELAY);
			a->add_lf = IsDlgButtonChecked(d, IDC_A_ADDLF) == BST_CHECKED;
			a->seven_bit = IsDlgButtonChecked(d, IDC_A_7BIT) == BST_CHECKED;
			a->wrap = IsDlgButtonChecked(d, IDC_A_WRAP) == BST_CHECKED;
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

void ascii_dialog(HWND parent)
{
	AsciiSetup a;
	ascii_load(&a);
	if (DialogBoxParam(app.inst, MAKEINTRESOURCE(IDD_ASCII), parent,
			   ascii_proc, (LPARAM)&a) != IDOK)
		return;
	save(&a);
	ascii_apply(*term_line.view);
	ascii_apply(*remote_line.view);
}
