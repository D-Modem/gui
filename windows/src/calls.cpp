/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * calls.cpp - the Call menu's Call dialog: the sessions, as
 *             HyperTerminal's connection files have them (a name and a
 *             number, kept in [sessions] of dmodem.ini), the number to
 *             dial chosen or typed; and Wait for a Call (ATS0=1).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "resource.h"

#define MAX_SESSIONS 64

struct Session {
	char name[64];
	char number[128];
};

static Session sessions[MAX_SESSIONS];
static int n_sessions;
static char *dial_out;                  /* the number chosen, for the caller */
static int dial_max;

static void ini(char *out, int n)
{
	char dir[MAX_PATH];
	app_data_dir(dir, sizeof(dir));
	_snprintf(out, n, "%s\\dmodem.ini", dir);
	out[n - 1] = '\0';
}

static void load()
{
	char path[MAX_PATH], key[32];
	ini(path, sizeof(path));
	n_sessions = 0;
	int n = (int)GetPrivateProfileInt("sessions", "count", 0, path);
	for (int i = 0; i < n && n_sessions < MAX_SESSIONS; i++) {
		Session *s = &sessions[n_sessions];
		_snprintf(key, sizeof(key), "name%d", i + 1);
		GetPrivateProfileString("sessions", key, "", s->name, sizeof(s->name), path);
		_snprintf(key, sizeof(key), "number%d", i + 1);
		GetPrivateProfileString("sessions", key, "", s->number, sizeof(s->number), path);
		if (s->name[0])
			n_sessions++;
	}
}

static void save()
{
	char path[MAX_PATH], key[32], v[16];
	ini(path, sizeof(path));
	/* (the section written again: no entries left from before) */
	WritePrivateProfileString("sessions", NULL, NULL, path);
	_snprintf(v, sizeof(v), "%d", n_sessions);
	WritePrivateProfileString("sessions", "count", v, path);
	for (int i = 0; i < n_sessions; i++) {
		_snprintf(key, sizeof(key), "name%d", i + 1);
		WritePrivateProfileString("sessions", key, sessions[i].name, path);
		_snprintf(key, sizeof(key), "number%d", i + 1);
		WritePrivateProfileString("sessions", key, sessions[i].number, path);
	}
}

static int find(const char *name)
{
	for (int i = 0; i < n_sessions; i++)
		if (!lstrcmpi(sessions[i].name, name))
			return i;
	return -1;
}

static void fill(HWND d, const char *sel)
{
	HWND c = GetDlgItem(d, IDC_C_NAME);
	SendMessage(c, CB_RESETCONTENT, 0, 0);
	for (int i = 0; i < n_sessions; i++)
		SendMessage(c, CB_ADDSTRING, 0, (LPARAM)sessions[i].name);
	SetWindowText(c, sel);
}

/* the session chosen in the list: its number */
static void picked(HWND d)
{
	int i = (int)SendDlgItemMessage(d, IDC_C_NAME, CB_GETCURSEL, 0, 0);
	char name[64];
	if (i < 0)
		return;
	SendDlgItemMessage(d, IDC_C_NAME, CB_GETLBTEXT, (WPARAM)i, (LPARAM)name);
	int k = find(name);
	if (k >= 0)
		SetDlgItemText(d, IDC_C_NUMBER, sessions[k].number);
}

static INT_PTR CALLBACK call_proc(HWND d, UINT m, WPARAM wp, LPARAM lp)
{
	(void)lp;
	char name[64], number[128];
	switch (m) {
	case WM_INITDIALOG:
		fill(d, n_sessions ? sessions[0].name : "");
		if (n_sessions)
			SetDlgItemText(d, IDC_C_NUMBER, sessions[0].number);
		return TRUE;
	case WM_COMMAND:
		GetDlgItemText(d, IDC_C_NAME, name, sizeof(name));
		GetDlgItemText(d, IDC_C_NUMBER, number, sizeof(number));
		switch (LOWORD(wp)) {
		case IDC_C_NAME:
			if (HIWORD(wp) == CBN_SELCHANGE)
				picked(d);
			return TRUE;
		case IDC_C_SAVE: {
			if (!name[0] || !number[0]) {
				MessageBox(d, "Type a name and a number to save the session.",
					   "Call", MB_OK | MB_ICONINFORMATION);
				return TRUE;
			}
			int k = find(name);
			if (k < 0) {
				if (n_sessions >= MAX_SESSIONS)
					return TRUE;
				k = n_sessions++;
			}
			_snprintf(sessions[k].name, sizeof(sessions[k].name), "%s", name);
			_snprintf(sessions[k].number, sizeof(sessions[k].number), "%s", number);
			save();
			fill(d, name);
			return TRUE;
		}
		case IDC_C_DELETE: {
			int k = find(name);
			if (k < 0)
				return TRUE;
			memmove(&sessions[k], &sessions[k + 1],
				(size_t)(n_sessions - k - 1) * sizeof(Session));
			n_sessions--;
			save();
			fill(d, "");
			SetDlgItemText(d, IDC_C_NUMBER, "");
			return TRUE;
		}
		case IDOK:
			if (!number[0]) {
				MessageBox(d, "Type a number or a SIP address (sip:user@host) to call.",
					   "Call", MB_OK | MB_ICONINFORMATION);
				return TRUE;
			}
			_snprintf(dial_out, dial_max, "%s", number);
			dial_out[dial_max - 1] = '\0';
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

bool call_dialog(HWND parent, char *number, int n)
{
	load();
	dial_out = number;
	dial_max = n;
	return DialogBoxParam(app.inst, MAKEINTRESOURCE(IDD_CALL), parent,
			      call_proc, 0) == IDOK;
}

static bool waiting;

bool call_waiting()
{
	return waiting;
}

static void wait_done(const dmc_event *ev, void *data)
{
	(void)data;
	if (!ev->ok) {
		waiting = false;
		app_message(ev->result);
	}
}

void call_wait(bool on)
{
	waiting = on;
	if (!link_open()) {
		/* (sent when the modem opens: call_opened) */
		return;
	}
	link_command(on ? "ATS0=1" : "ATS0=0", 0, wait_done, NULL);
	app_message(on ? "Waiting for a call: the modem answers at the first ring" :
		    "Not waiting for a call");
}

void call_opened()
{
	if (waiting)
		link_command("ATS0=1", 0, wait_done, NULL);
}
