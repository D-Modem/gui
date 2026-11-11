/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * config.cpp - the line settings, kept in dmodem.ini (in
 *              %APPDATA%\D-Modem, or beside the app where there is no
 *              APPDATA, as on Windows 9x), and the Line Settings dialog
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include <commdlg.h>
#include "resource.h"

#ifndef OPENFILENAME_SIZE_VERSION_400
#define OPENFILENAME_SIZE_VERSION_400 76
#endif

void app_data_dir(char *out, int n)
{
	char dir[MAX_PATH];
	DWORD k = GetEnvironmentVariable("APPDATA", dir, sizeof(dir));
	if (k > 0 && k < sizeof(dir) - 10) {
		strcat(dir, "\\D-Modem");
		CreateDirectory(dir, NULL);
		_snprintf(out, n, "%s", dir);
	} else {
		GetModuleFileName(NULL, out, n);
		char *slash = strrchr(out, '\\');
		if (slash)
			*slash = '\0';
	}
	out[n - 1] = '\0';
}

static void ini_path(char *out, int n)
{
	char dir[MAX_PATH];
	app_data_dir(dir, sizeof(dir));
	_snprintf(out, n, "%s\\dmodem.ini", dir);
	out[n - 1] = '\0';
}

static void get(const char *ini, const char *key, const char *def, char *out,
		int n)
{
	GetPrivateProfileString("line", key, def, out, n, ini);
}

static int get_i(const char *ini, const char *key, int def)
{
	return (int)GetPrivateProfileInt("line", key, def, ini);
}

static void put(const char *ini, const char *key, const char *v)
{
	WritePrivateProfileString("line", key, v, ini);
}

static void put_i(const char *ini, const char *key, int v)
{
	char b[16];
	_snprintf(b, sizeof(b), "%d", v);
	b[sizeof(b) - 1] = '\0';
	put(ini, key, b);
}

void config_load(Config *c)
{
	char ini[MAX_PATH];
	ini_path(ini, sizeof(ini));
	memset(c, 0, sizeof(*c));
	/* (Windows 9x and NT 4 can't run dmodemd: a COM port by default) */
	OSVERSIONINFO v;
	memset(&v, 0, sizeof(v));
	v.dwOSVersionInfoSize = sizeof(v);
	GetVersionEx(&v);
	bool old = v.dwPlatformId != VER_PLATFORM_WIN32_NT || v.dwMajorVersion < 6;
	c->line = get_i(ini, "line", old ? LINE_COM : LINE_DMODEMD);
	get(ini, "sip-user", "", c->sip_user, sizeof(c->sip_user));
	get(ini, "sip-password", "", c->sip_password, sizeof(c->sip_password));
	get(ini, "sip-server", "", c->sip_server, sizeof(c->sip_server));
	c->listen = get_i(ini, "listen", 0);
	c->t38 = get_i(ini, "t38", 1);
	c->v152 = get_i(ini, "v152", 1);
	c->v150 = get_i(ini, "v150", 0);
	get(ini, "country", "", c->country, sizeof(c->country));
	get(ini, "usb", "", c->usb, sizeof(c->usb));
	get(ini, "dmodemd", "", c->dmodemd, sizeof(c->dmodemd));
	get(ini, "tcp", "127.0.0.1:2323", c->tcp, sizeof(c->tcp));
	get(ini, "com", "COM1", c->com, sizeof(c->com));
	get(ini, "init", "", c->init, sizeof(c->init));
	/* the answering machine: off until the user turns it on */
	c->voicemail = (int)GetPrivateProfileInt("voice", "voicemail", 0, ini);
	c->vm_rings = (int)GetPrivateProfileInt("voice", "rings", 4, ini);
	if (c->vm_rings < 1 || c->vm_rings > 15)
		c->vm_rings = 4;
	c->vm_max = (int)GetPrivateProfileInt("voice", "max-seconds", 120, ini);
	if (c->vm_max < 10)
		c->vm_max = 120;
}

void config_save(const Config *c)
{
	char ini[MAX_PATH];
	ini_path(ini, sizeof(ini));
	put_i(ini, "line", c->line);
	put(ini, "sip-user", c->sip_user);
	put(ini, "sip-password", c->sip_password);
	put(ini, "sip-server", c->sip_server);
	put_i(ini, "listen", c->listen);
	put_i(ini, "t38", c->t38);
	put_i(ini, "v152", c->v152);
	put_i(ini, "v150", c->v150);
	put(ini, "country", c->country);
	put(ini, "usb", c->usb);
	put(ini, "dmodemd", c->dmodemd);
	put(ini, "tcp", c->tcp);
	put(ini, "com", c->com);
	put(ini, "init", c->init);
	char b[16];
	_snprintf(b, sizeof(b), "%d", c->voicemail);
	b[sizeof(b) - 1] = '\0';
	WritePrivateProfileString("voice", "voicemail", b, ini);
	_snprintf(b, sizeof(b), "%d", c->vm_rings);
	b[sizeof(b) - 1] = '\0';
	WritePrivateProfileString("voice", "rings", b, ini);
	_snprintf(b, sizeof(b), "%d", c->vm_max);
	b[sizeof(b) - 1] = '\0';
	WritePrivateProfileString("voice", "max-seconds", b, ini);
}

void config_summary(const Config *c, char *out, int n)
{
	switch (c->line) {
	case LINE_TCP:
		_snprintf(out, n, "dmodemd at %s", c->tcp);
		break;
	case LINE_COM:
		_snprintf(out, n, "The modem on %s", c->com);
		break;
	default:
		if (c->usb[0])
			_snprintf(out, n, "dmodemd: USB modem (%s)", c->usb);
		else if (!c->sip_user[0] || !c->sip_server[0])
			_snprintf(out, n, "dmodemd: SIP account not set up");
		else
			_snprintf(out, n, "dmodemd: SIP %s@%s%s", c->sip_user,
				  c->sip_server, c->listen ? ", taking calls" : "");
		break;
	}
	out[n - 1] = '\0';
}

/*
 *    the dialog
 */

static Config *editing;

static void enable(HWND d)
{
	bool here = IsDlgButtonChecked(d, IDC_L_DMODEMD) == BST_CHECKED;
	bool tcp = IsDlgButtonChecked(d, IDC_L_TCP) == BST_CHECKED;
	bool com = IsDlgButtonChecked(d, IDC_L_COM) == BST_CHECKED;
	static const int dmodemd_ids[] = {
		IDC_L_USER, IDC_L_PASSWORD, IDC_L_SERVER, IDC_L_LISTEN,
		IDC_L_T38, IDC_L_V152, IDC_L_V150, IDC_L_COUNTRY, IDC_L_USB,
		IDC_L_PATH, IDC_L_BROWSE, 0
	};
	for (int i = 0; dmodemd_ids[i]; i++)
		EnableWindow(GetDlgItem(d, dmodemd_ids[i]), here);
	EnableWindow(GetDlgItem(d, IDC_L_ADDRESS), tcp);
	EnableWindow(GetDlgItem(d, IDC_L_PORT), com);
}

static void check(HWND d, int id, int on)
{
	CheckDlgButton(d, id, on ? BST_CHECKED : BST_UNCHECKED);
}

static int checked(HWND d, int id)
{
	return IsDlgButtonChecked(d, id) == BST_CHECKED;
}

static void browse(HWND d)
{
	char file[MAX_PATH] = "dmodemd.exe";
	OPENFILENAME o;
	memset(&o, 0, sizeof(o));
	/* (the size Windows 95 and NT 4 know) */
	o.lStructSize = OPENFILENAME_SIZE_VERSION_400;
	o.hwndOwner = d;
	o.lpstrFilter = "dmodemd.exe\0dmodemd.exe\0Programs\0*.exe\0";
	o.lpstrFile = file;
	o.nMaxFile = sizeof(file);
	o.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
	if (GetOpenFileName(&o))
		SetDlgItemText(d, IDC_L_PATH, file);
}

static BOOL CALLBACK line_proc(HWND d, UINT m, WPARAM wp, LPARAM lp)
{
	(void)lp;
	Config *c = editing;
	switch (m) {
	case WM_INITDIALOG: {
		check(d, IDC_L_DMODEMD, c->line == LINE_DMODEMD);
		check(d, IDC_L_TCP, c->line == LINE_TCP);
		check(d, IDC_L_COM, c->line == LINE_COM);
		SetDlgItemText(d, IDC_L_USER, c->sip_user);
		SetDlgItemText(d, IDC_L_PASSWORD, c->sip_password);
		SetDlgItemText(d, IDC_L_SERVER, c->sip_server);
		check(d, IDC_L_LISTEN, c->listen);
		check(d, IDC_L_T38, c->t38);
		check(d, IDC_L_V152, c->v152);
		check(d, IDC_L_V150, c->v150);
		SetDlgItemText(d, IDC_L_COUNTRY, c->country);
		SetDlgItemText(d, IDC_L_USB, c->usb);
		SetDlgItemText(d, IDC_L_PATH, c->dmodemd);
		SetDlgItemText(d, IDC_L_ADDRESS, c->tcp);
		HWND port = GetDlgItem(d, IDC_L_PORT);
		for (int i = 1; i <= 16; i++) {
			char n[8];
			_snprintf(n, sizeof(n), "COM%d", i);
			n[sizeof(n) - 1] = '\0';
			SendMessage(port, CB_ADDSTRING, 0, (LPARAM)n);
		}
		SetWindowText(port, c->com);
		SetDlgItemText(d, IDC_L_INIT, c->init);
		enable(d);
		return TRUE;
	}
	case WM_COMMAND:
		switch (LOWORD(wp)) {
		case IDC_L_DMODEMD:
		case IDC_L_TCP:
		case IDC_L_COM:
			enable(d);
			return TRUE;
		case IDC_L_BROWSE:
			browse(d);
			return TRUE;
		case IDOK:
			c->line = checked(d, IDC_L_TCP) ? LINE_TCP :
				checked(d, IDC_L_COM) ? LINE_COM : LINE_DMODEMD;
			GetDlgItemText(d, IDC_L_USER, c->sip_user, sizeof(c->sip_user));
			GetDlgItemText(d, IDC_L_PASSWORD, c->sip_password,
				       sizeof(c->sip_password));
			GetDlgItemText(d, IDC_L_SERVER, c->sip_server,
				       sizeof(c->sip_server));
			c->listen = checked(d, IDC_L_LISTEN);
			c->t38 = checked(d, IDC_L_T38);
			c->v152 = checked(d, IDC_L_V152);
			c->v150 = checked(d, IDC_L_V150);
			GetDlgItemText(d, IDC_L_COUNTRY, c->country, sizeof(c->country));
			GetDlgItemText(d, IDC_L_USB, c->usb, sizeof(c->usb));
			GetDlgItemText(d, IDC_L_PATH, c->dmodemd, sizeof(c->dmodemd));
			GetDlgItemText(d, IDC_L_ADDRESS, c->tcp, sizeof(c->tcp));
			GetDlgItemText(d, IDC_L_PORT, c->com, sizeof(c->com));
			GetDlgItemText(d, IDC_L_INIT, c->init, sizeof(c->init));
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

bool line_dialog(HWND owner, Config *c)
{
	Config copy = *c;
	editing = &copy;
	if (DialogBox(app.inst, MAKEINTRESOURCE(IDD_LINE), owner,
		      (DLGPROC)line_proc) != IDOK)
		return false;
	*c = copy;
	return true;
}
