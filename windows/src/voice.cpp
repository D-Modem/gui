/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * voice.cpp - the Phone page: voice calls dialled and answered, their
 *             sound on the sound card, the keypad's DTMF; the answering
 *             machine (off until the user turns it on), its greeting
 *             and the messages it took, kept as WAV files in the app's
 *             folder (%APPDATA%\D-Modem\Voicemail)
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

/* the longest greeting recorded (s), the messages listed */
#define GREETING_MAX_S 30
#define MAX_MESSAGES 200

static HWND page;
static int last_state = DMC_CLOSED;
static bool muted, listening, listen_on;
/* what plays (a message, the greeting), and what is recorded */
static short *play_pcm;
static int play_n, play_pos;
static bool play_done_posted;
static short *rec_pcm;
static int rec_n;
static bool recording;
/* the greeting, 8 kHz */
static short *greeting;
static int greeting_n;
/* the messages' file names, as the list shows them */
static char names[MAX_MESSAGES][64];
static int name_count;

#define WM_APP_PLAYED (WM_APP + 3)

/*
 *    files
 */

static void messages_dir(char *out, int n)
{
	char d[MAX_PATH];
	app_data_dir(d, sizeof(d));
	_snprintf(out, n, "%s\\Voicemail", d);
	out[n - 1] = '\0';
}

static void greeting_path(char *out, int n)
{
	char d[MAX_PATH];
	app_data_dir(d, sizeof(d));
	_snprintf(out, n, "%s\\greeting.wav", d);
	out[n - 1] = '\0';
}

static bool write_wav(const char *path, const short *pcm, int n)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return false;
	char h[44];
	dmc_wav_header(h, n);
	bool ok = fwrite(h, 1, 44, f) == 44;
	for (int i = 0; ok && i < n; i++) {
		unsigned char b[2];
		b[0] = (unsigned char)(pcm[i] & 0xff);
		b[1] = (unsigned char)((pcm[i] >> 8) & 0xff);
		ok = fwrite(b, 1, 2, f) == 2;
	}
	if (fclose(f))
		ok = false;
	return ok;
}

/* a WAV file's samples at 8 kHz: how many (*pcm to dmc_free()), -1 */
static int read_wav(const char *path, short **pcm)
{
	*pcm = NULL;
	FILE *f = fopen(path, "rb");
	if (!f)
		return -1;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len <= 0 || len > 64L * 1024 * 1024) {
		fclose(f);
		return -1;
	}
	char *b = (char *)malloc(len);
	if (!b) {
		fclose(f);
		return -1;
	}
	int n = -1;
	if (fread(b, 1, len, f) == (size_t)len)
		n = dmc_wav_decode(b, (int)len, (int16_t **)pcm);
	fclose(f);
	free(b);
	return n;
}

static void load_greeting()
{
	char p[MAX_PATH];
	greeting_path(p, sizeof(p));
	if (greeting)
		dmc_free(greeting);
	greeting = NULL;
	greeting_n = 0;
	short *pcm;
	int n = read_wav(p, &pcm);
	if (n > 0) {
		greeting = pcm;
		greeting_n = n;
	} else if (pcm) {
		dmc_free(pcm);
	}
}

void voice_configure()
{
	dmc_voicemail_set(app.s, (const int16_t *)greeting, greeting_n,
			  app.cfg.vm_max, 0);
	dmc_voicemail_auto(app.s, app.cfg.voicemail ? app.cfg.vm_rings : 0);
}

/*
 *    sound
 */

static int call_pull(short *pcm, int n)
{
	return dmc_voice_read(app.s, (int16_t *)pcm, n);
}

static void call_push(const short *pcm, int n)
{
	if (!muted)
		dmc_voice_write(app.s, (const int16_t *)pcm, n);
}

static int play_pull(short *pcm, int n)
{
	int k = play_n - play_pos;
	if (k > n)
		k = n;
	if (k > 0) {
		memcpy(pcm, play_pcm + play_pos, k * sizeof(short));
		play_pos += k;
	}
	/* (stopped from the message loop, not inside the device's) */
	if (play_pos >= play_n && !play_done_posted) {
		play_done_posted = true;
		PostMessage(page, WM_APP_PLAYED, 0, 0);
	}
	return k;
}

static void record_push(const short *pcm, int n)
{
	int room = GREETING_MAX_S * DMC_VOICE_RATE - rec_n;
	if (n > room)
		n = room;
	if (n > 0 && rec_pcm) {
		memcpy(rec_pcm + rec_n, pcm, n * sizeof(short));
		rec_n += n;
	}
}

static void stop_sound()
{
	audio_stop();
	if (play_pcm)
		dmc_free(play_pcm);
	play_pcm = NULL;
	play_n = play_pos = 0;
	free(rec_pcm);
	rec_pcm = NULL;
	rec_n = 0;
	recording = false;
}

/* pcm (taken: dmc_free()d when done) played */
static void play(short *pcm, int n)
{
	stop_sound();
	play_pcm = pcm;
	play_n = n;
	play_pos = 0;
	play_done_posted = false;
	if (!audio_start(play_pull, NULL)) {
		app_message("No sound card to play it on");
		stop_sound();
	}
	voice_state();
}

/*
 *    the messages
 */

static void refresh_messages();

static void insert_column(HWND lv, int i, const char *t, int w)
{
	LVCOLUMN c;
	memset(&c, 0, sizeof(c));
	c.mask = LVCF_TEXT | LVCF_WIDTH;
	c.pszText = (char *)t;
	c.cx = w;
	ListView_InsertColumn(lv, i, &c);
}

static int newest_first(const void *x, const void *y)
{
	return -strcmp((const char *)x, (const char *)y);
}

/* the list from the files: 20261111-120501_5551234.wav */
static void refresh_messages()
{
	HWND lv = GetDlgItem(page, IDC_V_MESSAGES);
	ListView_DeleteAllItems(lv);
	name_count = 0;
	char dir[MAX_PATH], pat[MAX_PATH];
	messages_dir(dir, sizeof(dir));
	_snprintf(pat, sizeof(pat), "%s\\*.wav", dir);
	pat[sizeof(pat) - 1] = '\0';
	WIN32_FIND_DATA fd;
	HANDLE h = FindFirstFile(pat, &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			if (name_count < MAX_MESSAGES &&
			    strlen(fd.cFileName) < sizeof(names[0])) {
				strcpy(names[name_count], fd.cFileName);
				name_count++;
			}
		} while (FindNextFile(h, &fd));
		FindClose(h);
	}
	qsort(names, name_count, sizeof(names[0]), newest_first);
	int row = 0;
	for (int i = 0; i < name_count; i++) {
		int y, mo, da, hh, mi, se;
		char who[64] = "";
		if (sscanf(names[i], "%4d%2d%2d-%2d%2d%2d_%63[^.]", &y, &mo,
			   &da, &hh, &mi, &se, who) < 6)
			continue;
		char path[MAX_PATH], when[64], len[32];
		_snprintf(path, sizeof(path), "%s\\%s", dir, names[i]);
		path[sizeof(path) - 1] = '\0';
		long secs = 0;
		FILE *f = fopen(path, "rb");
		if (f) {
			fseek(f, 0, SEEK_END);
			long bytes = ftell(f);
			fclose(f);
			if (bytes > 44)
				secs = (bytes - 44) / (2 * DMC_VOICE_RATE);
		}
		_snprintf(when, sizeof(when), "%04d-%02d-%02d %02d:%02d", y, mo,
			  da, hh, mi);
		when[sizeof(when) - 1] = '\0';
		_snprintf(len, sizeof(len), "%ld:%02ld", secs / 60, secs % 60);
		len[sizeof(len) - 1] = '\0';
		LVITEM it;
		memset(&it, 0, sizeof(it));
		it.mask = LVIF_TEXT | LVIF_PARAM;
		it.iItem = row;
		it.pszText = who[0] ? who : (char *)"Unknown caller";
		it.lParam = i;
		ListView_InsertItem(lv, &it);
		ListView_SetItemText(lv, row, 1, when);
		ListView_SetItemText(lv, row, 2, len);
		row++;
	}
}

/* the selected message's path: false if none is */
static bool selected(char *path, int n)
{
	HWND lv = GetDlgItem(page, IDC_V_MESSAGES);
	int row = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
	if (row < 0)
		return false;
	LVITEM it;
	memset(&it, 0, sizeof(it));
	it.mask = LVIF_PARAM;
	it.iItem = row;
	if (!ListView_GetItem(lv, &it) || it.lParam < 0 ||
	    it.lParam >= name_count)
		return false;
	char dir[MAX_PATH];
	messages_dir(dir, sizeof(dir));
	_snprintf(path, n, "%s\\%s", dir, names[it.lParam]);
	path[n - 1] = '\0';
	return true;
}

static void save_message(const char *number)
{
	const int16_t *pcm;
	int n = dmc_voice_message(app.s, &pcm);
	if (n <= 0)
		return;
	char who[40];
	int k = 0;
	for (const char *p = number; *p && k < 32; p++)
		if ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') ||
		    (*p >= 'a' && *p <= 'z') || *p == '+' || *p == '#')
			who[k++] = *p;
	who[k] = '\0';
	SYSTEMTIME t;
	GetLocalTime(&t);
	char dir[MAX_PATH], path[MAX_PATH];
	messages_dir(dir, sizeof(dir));
	CreateDirectory(dir, NULL);
	_snprintf(path, sizeof(path), "%s\\%04d%02d%02d-%02d%02d%02d_%s.wav",
		  dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
		  t.wSecond, who);
	path[sizeof(path) - 1] = '\0';
	if (!write_wav(path, (const short *)pcm, n)) {
		app_message("Can't keep the message");
		return;
	}
	char m[96];
	_snprintf(m, sizeof(m), "New voice message from %s",
		  who[0] ? who : "an unknown caller");
	m[sizeof(m) - 1] = '\0';
	app_message(m);
	app_log("The answering machine took a message.");
	if (GetForegroundWindow() != app.main)
		FlashWindow(app.main, TRUE);
	refresh_messages();
}

/*
 *    the greeting
 */

static void record_greeting()
{
	if (recording) {
		/* stopped: kept */
		short *pcm = rec_pcm;
		int n = rec_n;
		rec_pcm = NULL;
		stop_sound();
		if (n > DMC_VOICE_RATE / 2) {
			char p[MAX_PATH];
			greeting_path(p, sizeof(p));
			if (!write_wav(p, pcm, n))
				app_message("Can't keep the greeting");
			load_greeting();
			voice_configure();
		}
		free(pcm);
		voice_state();
		return;
	}
	stop_sound();
	rec_pcm = (short *)malloc(GREETING_MAX_S * DMC_VOICE_RATE *
				  sizeof(short));
	rec_n = 0;
	recording = rec_pcm != NULL;
	if (recording && !audio_start(NULL, record_push)) {
		app_message("No microphone to record with");
		stop_sound();
	}
	voice_state();
}

static void choose_greeting()
{
	char file[MAX_PATH] = "";
	OPENFILENAME o;
	memset(&o, 0, sizeof(o));
	/* (the size Windows 95 and NT 4 know) */
	o.lStructSize = OPENFILENAME_SIZE_VERSION_400;
	o.hwndOwner = app.main;
	o.lpstrFilter = "WAV files\0*.wav\0";
	o.lpstrFile = file;
	o.nMaxFile = sizeof(file);
	o.lpstrTitle = "Greeting";
	o.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
	if (!GetOpenFileName(&o))
		return;
	short *pcm;
	int n = read_wav(file, &pcm);
	if (n <= 0) {
		if (pcm)
			dmc_free(pcm);
		app_message("That isn't a WAV file D-Modem can play");
		return;
	}
	char p[MAX_PATH];
	greeting_path(p, sizeof(p));
	if (!write_wav(p, pcm, n))
		app_message("Can't keep the greeting");
	dmc_free(pcm);
	load_greeting();
	voice_configure();
	voice_state();
}

/*
 *    calls
 */

static void dialled(const dmc_event *ev, void *data)
{
	(void)data;
	if (!ev->ok)
		app_message(!strcmp(ev->result, "ERROR") ?
			    "The modem has no voice mode" : ev->result);
}

static void voice_command(HWND p, int id, int code)
{
	(void)code;
	if (id >= IDC_V_KEY && id < IDC_V_KEY + 12) {
		static const char keys[] = "123456789*0#";
		char k = keys[id - IDC_V_KEY];
		if (dmc_state(app.s) == DMC_VOICE) {
			dmc_voice_dtmf(app.s, k);
		} else {
			HWND e = GetDlgItem(p, IDC_V_NUMBER);
			char s[2] = { k, 0 };
			int len = GetWindowTextLength(e);
			SendMessage(e, EM_SETSEL, len, len);
			SendMessage(e, EM_REPLACESEL, FALSE, (LPARAM)s);
		}
		return;
	}
	switch (id) {
	case IDC_V_CALL: {
		char n[128];
		GetDlgItemText(p, IDC_V_NUMBER, n, sizeof(n));
		if (!n[0]) {
			app_message("Enter a number to call");
			return;
		}
		stop_sound();
		int tag = dmc_voice_dial(app.s, n);
		if (tag > 0)
			link_wait(tag, dialled, NULL);
		break;
	}
	case IDC_V_ANSWER: {
		stop_sound();
		int tag = dmc_voice_answer(app.s);
		if (tag > 0)
			link_wait(tag, dialled, NULL);
		break;
	}
	case IDC_V_MESSAGE: {
		stop_sound();
		int tag = dmc_voicemail_answer(app.s);
		if (tag > 0)
			link_wait(tag, dialled, NULL);
		break;
	}
	case IDC_V_PICKUP:
		dmc_voice_pickup(app.s);
		link_pump();
		break;
	case IDC_V_HANGUP:
		dmc_hangup(app.s);
		link_pump();
		break;
	case IDC_V_MUTE:
		muted = IsDlgButtonChecked(p, IDC_V_MUTE) == BST_CHECKED;
		break;
	case IDC_V_LISTEN:
		listening = IsDlgButtonChecked(p, IDC_V_LISTEN) == BST_CHECKED;
		voice_state();
		break;
	case IDC_V_VMON:
		app.cfg.voicemail = IsDlgButtonChecked(p, IDC_V_VMON) ==
			BST_CHECKED;
		config_save(&app.cfg);
		voice_configure();
		voice_state();
		break;
	case IDC_V_RINGS:
		if (code == CBN_SELCHANGE) {
			int i = (int)SendDlgItemMessage(p, IDC_V_RINGS,
							CB_GETCURSEL, 0, 0);
			if (i >= 0) {
				app.cfg.vm_rings = i + 1;
				config_save(&app.cfg);
				voice_configure();
			}
		}
		break;
	case IDC_V_RECORD:
		record_greeting();
		break;
	case IDC_V_PLAYGREET:
		if (play_pcm) {
			stop_sound();
		} else if (greeting_n) {
			short *c = (short *)malloc(greeting_n * sizeof(short));
			if (c) {
				memcpy(c, greeting, greeting_n * sizeof(short));
				play(c, greeting_n);
			}
		}
		voice_state();
		break;
	case IDC_V_CHOOSE:
		choose_greeting();
		break;
	case IDC_V_CLEARGREET: {
		char g[MAX_PATH];
		greeting_path(g, sizeof(g));
		DeleteFile(g);
		load_greeting();
		voice_configure();
		voice_state();
		break;
	}
	case IDC_V_PLAY: {
		char path[MAX_PATH];
		if (play_pcm) {
			stop_sound();
			voice_state();
			break;
		}
		if (!selected(path, sizeof(path))) {
			app_message("Select a message to play");
			break;
		}
		short *pcm;
		int n = read_wav(path, &pcm);
		if (n > 0) {
			play(pcm, n);
		} else {
			if (pcm)
				dmc_free(pcm);
			app_message("Can't read the message");
		}
		break;
	}
	case IDC_V_DELETE: {
		char path[MAX_PATH];
		if (!selected(path, sizeof(path)))
			break;
		stop_sound();
		if (!DeleteFile(path))
			app_message("Can't delete the message");
		refresh_messages();
		voice_state();
		break;
	}
	}
}

/* the sound as the call needs it: both ways in a call; the caller (if
   listening) while the answering machine has the call; the 20 ms timer
   of a call */
static void call_sound(int st)
{
	int was = last_state;
	bool in = st == DMC_VOICE || st == DMC_VOICEMAIL;
	bool listen = st == DMC_VOICEMAIL && listening;
	if (in)
		SetTimer(app.main, TIMER_VOICE, 20, NULL);
	else if (st != DMC_HANGING_UP)
		KillTimer(app.main, TIMER_VOICE);
	if (st == DMC_VOICE && was != DMC_VOICE) {
		stop_sound();
		listen_on = false;
		if (!audio_start(call_pull, call_push))
			app_message("No sound card: the call has no sound");
	} else if (listen && !listen_on) {
		audio_start(call_pull, NULL);
		listen_on = true;
	} else if (!listen && listen_on) {
		audio_stop();
		listen_on = false;
	} else if (was == DMC_VOICE && st != DMC_VOICE) {
		audio_stop();
	}
	last_state = st;
}

void voice_state()
{
	if (!page)
		return;
	int st = dmc_state(app.s);
	call_sound(st);
	char s[160];
	const char *who = dmc_caller_number(app.s);
	switch (st) {
	case DMC_CLOSED:
		strcpy(s, "The modem isn't running");
		break;
	case DMC_RINGING:
		_snprintf(s, sizeof(s), "Ringing%s%s", who[0] ? ": " : "", who);
		break;
	case DMC_VOICE:
		strcpy(s, "In a call");
		break;
	case DMC_VOICEMAIL:
		_snprintf(s, sizeof(s), "Taking a message%s%s",
			  who[0] ? " from " : "", who);
		break;
	default:
		_snprintf(s, sizeof(s), "%s", dmc_state_name(st));
		break;
	}
	s[sizeof(s) - 1] = '\0';
	char a[200];
	to_ansi(s, a, sizeof(a));
	SetDlgItemText(page, IDC_V_STATUS, a);
	bool idle = st == DMC_IDLE;
	EnableWindow(GetDlgItem(page, IDC_V_CALL), idle);
	EnableWindow(GetDlgItem(page, IDC_V_ANSWER), st == DMC_RINGING);
	EnableWindow(GetDlgItem(page, IDC_V_MESSAGE), st == DMC_RINGING);
	EnableWindow(GetDlgItem(page, IDC_V_PICKUP), st == DMC_VOICEMAIL);
	EnableWindow(GetDlgItem(page, IDC_V_LISTEN), st == DMC_VOICEMAIL);
	EnableWindow(GetDlgItem(page, IDC_V_HANGUP),
		     st == DMC_VOICE || st == DMC_VOICEMAIL ||
		     st == DMC_DIALING || st == DMC_ANSWERING);
	EnableWindow(GetDlgItem(page, IDC_V_MUTE), st == DMC_VOICE);
	bool keys = st == DMC_VOICE || idle || st == DMC_CLOSED;
	for (int i = 0; i < 12; i++)
		EnableWindow(GetDlgItem(page, IDC_V_KEY + i), keys);
	EnableWindow(GetDlgItem(page, IDC_V_RINGS), app.cfg.voicemail);
	bool in = st == DMC_VOICE || st == DMC_VOICEMAIL;
	SetDlgItemText(page, IDC_V_RECORD, recording ? "S&top" : "&Record");
	EnableWindow(GetDlgItem(page, IDC_V_RECORD), !play_pcm && !in);
	SetDlgItemText(page, IDC_V_PLAYGREET, play_pcm ? "St&op" : "&Play");
	EnableWindow(GetDlgItem(page, IDC_V_PLAYGREET),
		     (greeting_n > 0 || play_pcm) && !recording && !in);
	EnableWindow(GetDlgItem(page, IDC_V_CLEARGREET),
		     greeting_n > 0 && !recording && !play_pcm);
	SetDlgItemText(page, IDC_V_PLAY, play_pcm ? "St&op" : "P&lay");
	EnableWindow(GetDlgItem(page, IDC_V_PLAY), !recording && !in);
	char g[96];
	if (recording)
		strcpy(g, "Greeting: recording; speak, then Stop");
	else if (greeting_n)
		_snprintf(g, sizeof(g), "Greeting: your recording (%d s)",
			  (greeting_n + DMC_VOICE_RATE / 2) / DMC_VOICE_RATE);
	else
		strcpy(g, "Greeting: none, callers hear a beep");
	g[sizeof(g) - 1] = '\0';
	SetDlgItemText(page, IDC_V_GREETING, g);
}

void voice_event(const dmc_event *ev)
{
	if (ev->type == DMC_EV_MESSAGE) {
		if (ev->ok)
			save_message(ev->text);
	} else if (ev->type == DMC_EV_VOICE) {
		if (!strcmp(ev->text, "b"))
			app_message("Busy");
		else if (!strcmp(ev->text, "l") &&
			 dmc_state(app.s) != DMC_VOICEMAIL)
			app_message("The other end hung up");
	}
}

/*
 *    the page
 */

static WNDPROC page_base;

/* the page's own messages: a message played to its end */
static LRESULT CALLBACK voice_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
	if (m == WM_APP_PLAYED) {
		stop_sound();
		voice_state();
		return 0;
	}
	return CallWindowProc(page_base, w, m, wp, lp);
}

HWND voice_page_new(HWND parent)
{
	page = page_new(parent, voice_command);
	page_base = (WNDPROC)SetWindowLongPtr(page, GWLP_WNDPROC,
					      (LONG_PTR)voice_proc);
	HWND st = make_child("STATIC", "", SS_LEFT | SS_NOPREFIX, page,
			     IDC_V_STATUS);
	SendMessage(st, WM_SETFONT, (WPARAM)app.bold_font, TRUE);
	make_child("BUTTON", "Voice Call", BS_GROUPBOX, page, IDC_V_CALLBOX);
	make_child("STATIC", "&Number:", SS_LEFT, page, IDC_V_NUMLABEL);
	make_child("EDIT", "", WS_TABSTOP | ES_AUTOHSCROLL, page, IDC_V_NUMBER,
		   WS_EX_CLIENTEDGE);
	make_child("BUTTON", "&Call", WS_TABSTOP | BS_DEFPUSHBUTTON, page,
		   IDC_V_CALL);
	make_child("BUTTON", "&Answer", WS_TABSTOP, page, IDC_V_ANSWER);
	make_child("BUTTON", "Take &Message", WS_TABSTOP, page, IDC_V_MESSAGE);
	make_child("BUTTON", "Pic&k Up", WS_TABSTOP, page, IDC_V_PICKUP);
	make_child("BUTTON", "&Hang Up", WS_TABSTOP, page, IDC_V_HANGUP);
	make_child("BUTTON", "M&ute", WS_TABSTOP | BS_AUTOCHECKBOX |
		   BS_PUSHLIKE, page, IDC_V_MUTE);
	make_child("BUTTON", "Li&sten", WS_TABSTOP | BS_AUTOCHECKBOX, page,
		   IDC_V_LISTEN);
	static const char keys[] = "123456789*0#";
	for (int i = 0; i < 12; i++) {
		char k[2] = { keys[i], 0 };
		make_child("BUTTON", k, WS_TABSTOP, page, IDC_V_KEY + i);
	}
	make_child("BUTTON", "Answering Machine", BS_GROUPBOX, page,
		   IDC_V_VMBOX);
	make_child("BUTTON", "&Take messages when nobody answers",
		   WS_TABSTOP | BS_AUTOCHECKBOX, page, IDC_V_VMON);
	make_child("STATIC", "after", SS_RIGHT, page, IDC_V_RINGSLABEL);
	HWND rings = make_child("COMBOBOX", "", WS_TABSTOP | CBS_DROPDOWNLIST |
				WS_VSCROLL, page, IDC_V_RINGS);
	for (int i = 1; i <= 10; i++) {
		char r[16];
		_snprintf(r, sizeof(r), i == 1 ? "%d ring" : "%d rings", i);
		r[sizeof(r) - 1] = '\0';
		SendMessage(rings, CB_ADDSTRING, 0, (LPARAM)r);
	}
	int sel = app.cfg.vm_rings >= 1 && app.cfg.vm_rings <= 10 ?
		app.cfg.vm_rings - 1 : 3;
	SendMessage(rings, CB_SETCURSEL, sel, 0);
	CheckDlgButton(page, IDC_V_VMON,
		       app.cfg.voicemail ? BST_CHECKED : BST_UNCHECKED);
	make_child("STATIC", "", SS_LEFT | SS_NOPREFIX, page, IDC_V_GREETING);
	make_child("BUTTON", "&Record", WS_TABSTOP, page, IDC_V_RECORD);
	make_child("BUTTON", "&Play", WS_TABSTOP, page, IDC_V_PLAYGREET);
	make_child("BUTTON", "&File...", WS_TABSTOP, page, IDC_V_CHOOSE);
	make_child("BUTTON", "&None", WS_TABSTOP, page, IDC_V_CLEARGREET);
	make_child("BUTTON", "Messages", BS_GROUPBOX, page, IDC_V_MSGBOX);
	HWND lv = make_child(WC_LISTVIEW, "", WS_TABSTOP | LVS_REPORT |
			     LVS_SINGLESEL | LVS_SHOWSELALWAYS, page,
			     IDC_V_MESSAGES, WS_EX_CLIENTEDGE);
	ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT);
	insert_column(lv, 0, "From", 160);
	insert_column(lv, 1, "When", 130);
	insert_column(lv, 2, "Length", 60);
	make_child("BUTTON", "P&lay", WS_TABSTOP, page, IDC_V_PLAY);
	make_child("BUTTON", "&Delete", WS_TABSTOP, page, IDC_V_DELETE);
	load_greeting();
	refresh_messages();
	voice_configure();
	return page;
}

static void place(HWND p, int id, int x, int y, int w, int h)
{
	MoveWindow(GetDlgItem(p, id), x, y, w, h, TRUE);
}

void voice_page_layout(HWND p, int w, int h)
{
	int m = 12, bw = 90, bh = 24, x = m, y = m;
	int inner = w - 2 * m;
	if (inner > 640)
		inner = 640;
	place(p, IDC_V_STATUS, x, y, inner, 20);
	y += 28;
	/* the call: the number, its buttons, the keypad at the right */
	int kw = 34, kh = 24, pad = 3 * kw + 2 * 4;
	int left = inner - pad - 36;
	place(p, IDC_V_CALLBOX, x, y, inner, 156);
	place(p, IDC_V_NUMLABEL, x + 12, y + 28, 56, 20);
	place(p, IDC_V_NUMBER, x + 70, y + 24, left - 70 - bw - 6, bh);
	place(p, IDC_V_CALL, x + left - bw, y + 24, bw, bh);
	place(p, IDC_V_ANSWER, x + 12, y + 58, bw, bh);
	place(p, IDC_V_MESSAGE, x + 18 + bw, y + 58, bw + 10, bh);
	place(p, IDC_V_HANGUP, x + left - bw, y + 58, bw, bh);
	place(p, IDC_V_PICKUP, x + 12, y + 90, bw, bh);
	place(p, IDC_V_LISTEN, x + 18 + bw, y + 92, bw + 10, 20);
	place(p, IDC_V_MUTE, x + left - bw, y + 90, bw, bh);
	for (int i = 0; i < 12; i++)
		place(p, IDC_V_KEY + i, x + inner - pad - 12 + (i % 3) * (kw + 4),
		      y + 20 + (i / 3) * (kh + 6), kw, kh);
	y += 166;
	/* the answering machine */
	place(p, IDC_V_VMBOX, x, y, inner, 84);
	place(p, IDC_V_VMON, x + 12, y + 24, 230, 20);
	place(p, IDC_V_RINGSLABEL, x + 246, y + 26, 34, 20);
	place(p, IDC_V_RINGS, x + 284, y + 22, 90, 200);
	int gb = 60;
	place(p, IDC_V_GREETING, x + 12, y + 56, inner - 4 * (gb + 6) - 24, 20);
	place(p, IDC_V_RECORD, x + inner - 4 * (gb + 6) - 6, y + 52, gb, bh);
	place(p, IDC_V_PLAYGREET, x + inner - 3 * (gb + 6) - 6, y + 52, gb, bh);
	place(p, IDC_V_CHOOSE, x + inner - 2 * (gb + 6) - 6, y + 52, gb, bh);
	place(p, IDC_V_CLEARGREET, x + inner - (gb + 6) - 6, y + 52, gb, bh);
	y += 94;
	/* the messages, the rest */
	int mh = h - y - m;
	if (mh < 90)
		mh = 90;
	place(p, IDC_V_MSGBOX, x, y, inner, mh);
	place(p, IDC_V_MESSAGES, x + 12, y + 22, inner - bw - 36, mh - 34);
	place(p, IDC_V_PLAY, x + inner - bw - 12, y + 22, bw, bh);
	place(p, IDC_V_DELETE, x + inner - bw - 12, y + 52, bw, bh);
}
