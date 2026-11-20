/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * xfer.cpp - file transfers on a terminal's line, as HyperTerminal's
 *            Transfer menu has them: Send File and Receive File with the
 *            protocols of libdmodemctl (dmc_xfer: ZMODEM, YMODEM, XMODEM,
 *            XMODEM-1K, Kermit, Intel HEX, S-records, plain text), a
 *            dialog of the transfer as it goes, and the remote's own
 *            starts noticed on the line (its sz or kermit -s: a receive
 *            here; its rz: the Send File dialog).
 *
 * One transfer at a time, on the Terminal page's modem (term.cpp) or
 * the Remote page's login (remote.cpp), each an XferLine.  The protocol
 * runs on a thread of its own; its bytes go to the line here, on the
 * app's thread (WM_APP_XFER when it has some, and the pump's timer).
 * The folder, the protocols last used and Automatic Receive are kept in
 * dmodem.ini ([transfer]).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "resource.h"

#include <commdlg.h>
#ifndef DMODEM_WINPORT
#include <shlobj.h>
#endif

#ifndef OFN_EXPLORER
#define OFN_EXPLORER 0x00080000
#endif
#ifndef PBM_SETRANGE
#define PBM_SETRANGE (WM_USER + 1)
#define PBM_SETPOS (WM_USER + 2)
#endif

#define MAX_FILES 64

static dmc_xfer *cur;
static XferLine *cur_line;
static HWND dlg, bar;
static DWORD started_at, last_shown;
static long long done_bytes;            /* the files finished before this */
static int last_files, total_files;
static bool bps;                        /* bits a second, else characters */
static char where[MAX_PATH];
static bool send_wanted;                /* the remote's rz noticed, not yet answered */

/* the protocols as the dialogs list them (HyperTerminal's "Zmodem with
   Crash Recovery" a ZMODEM with the resume option) */
static const struct Choice {
	int protocol;
	int resume;
} choices[] = {
	{ DMC_XFER_ZMODEM, 0 },
	{ DMC_XFER_ZMODEM, 1 },
	{ DMC_XFER_YMODEM, 0 },
	{ DMC_XFER_YMODEM_G, 0 },
	{ DMC_XFER_XMODEM, 0 },
	{ DMC_XFER_XMODEM_1K, 0 },
	{ DMC_XFER_KERMIT, 0 },
	{ DMC_XFER_INTEL_HEX, 0 },
	{ DMC_XFER_SREC, 0 },
	{ DMC_XFER_TEXT, 0 },
};
#define CHOICES ((int)(sizeof(choices) / sizeof(choices[0])))

static int choice_of(int protocol, int resume)
{
	for (int i = 0; i < CHOICES; i++)
		if (choices[i].protocol == protocol && choices[i].resume == (resume != 0))
			return i;
	return 0;
}

static void choice_label(int i, char *out, int n)
{
	int p = choices[i].protocol;
	_snprintf(out, n, "%s%s%s", dmc_xfer_label(p),
		  choices[i].resume ? " with Crash Recovery" : "",
		  dmc_xfer_batch(p) ? "" : " (one file)");
	out[n - 1] = '\0';
}

static void ini(char *out, int n)
{
	char dir[MAX_PATH];
	app_data_dir(dir, sizeof(dir));
	_snprintf(out, n, "%s" DIR_SEP "dmodem.ini", dir);
	out[n - 1] = '\0';
}

static int ini_int(const char *key, int def)
{
	char path[MAX_PATH];
	ini(path, sizeof(path));
	return (int)GetPrivateProfileInt("transfer", key, def, path);
}

/* ASCII Setup's ([ascii]: the text's line and character delays) */
static int ascii_int(const char *key, int def)
{
	char path[MAX_PATH];
	ini(path, sizeof(path));
	return (int)GetPrivateProfileInt("ascii", key, def, path);
}

static void ini_set(const char *key, const char *v)
{
	char path[MAX_PATH];
	ini(path, sizeof(path));
	WritePrivateProfileString("transfer", key, v, path);
}

static void ini_set_int(const char *key, int v)
{
	char s[16];
	_snprintf(s, sizeof(s), "%d", v);
	s[sizeof(s) - 1] = '\0';
	ini_set(key, s);
}

/* where received files go: the folder chosen last, else Received in the
   app's folder (made if it isn't there) */
static void folder(char *out, int n)
{
	char path[MAX_PATH];
	ini(path, sizeof(path));
	GetPrivateProfileString("transfer", "folder", "", out, n, path);
	if (out[0])
		return;
	char dir[MAX_PATH];
	app_data_dir(dir, sizeof(dir));
	_snprintf(out, n, "%s" DIR_SEP "Received", dir);
	out[n - 1] = '\0';
	CreateDirectory(out, NULL);
}

bool xfer_auto()
{
	return ini_int("auto", 1) != 0;
}

void xfer_set_auto(bool on)
{
	ini_set_int("auto", on ? 1 : 0);
}

bool xfer_zstd()
{
	return ini_int("zmodem_zstd", 1) != 0;
}

void xfer_set_zstd(bool on)
{
	ini_set_int("zmodem_zstd", on ? 1 : 0);
}

bool xfer_running()
{
	return cur != NULL;
}

bool xfer_busy(XferLine *l)
{
	return cur && cur_line == l;
}

/* ------------------------------------------------------------------ */
/* the transfer's dialog */

static void set_text(int id, const char *s)
{
	if (dlg)
		SetDlgItemText(dlg, id, s);
}

static void clock_text(DWORD ms, char *out, int n)
{
	DWORD s = ms / 1000;
	_snprintf(out, n, "%02lu:%02lu:%02lu", (unsigned long)(s / 3600),
		  (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
	out[n - 1] = '\0';
}

static void size_text(long long v, char *out, int n)
{
	if (v < 1024)
		_snprintf(out, n, "%ld bytes", (long)v);
	else if (v < 1024 * 1024)
		_snprintf(out, n, "%.1f KB", v / 1024.0);
	else
		_snprintf(out, n, "%.1f MB", v / 1048576.0);
	out[n - 1] = '\0';
}

static void show_status(const dmc_xfer_status *st, bool force)
{
	if (!dlg)
		return;
	DWORD now = GetTickCount();
	if (!force && now - last_shown < 250)
		return;
	last_shown = now;
	char a[300], b[64], c[64];
	to_ansi(st->file[0] ? st->file : "(waiting for the other side)", a,
		sizeof(a));
	/* (a file that goes compressed: ZMODEM with zstd) */
	if (st->running && cur && dmc_xfer_zstd(cur)) {
		size_t al = strlen(a);
		_snprintf(a + al, sizeof(a) - al, " (zstd)");
		a[sizeof(a) - 1] = '\0';
	}
	set_text(IDC_X_FILE, a);
	if (st->files > last_files) {
		done_bytes += st->size > 0 ? st->size : st->bytes;
		last_files = st->files;
	}
	if (st->sending && total_files > 0)
		_snprintf(a, sizeof(a), "%d of %d", st->files < total_files ?
			  st->files + 1 : total_files, total_files);
	else
		_snprintf(a, sizeof(a), "%d done", st->files);
	a[sizeof(a) - 1] = '\0';
	set_text(IDC_X_FILES, a);
	_snprintf(a, sizeof(a), "%d", st->errors);
	a[sizeof(a) - 1] = '\0';
	set_text(IDC_X_ERRORS, a);
	DWORD el = now - started_at;
	clock_text(el, a, sizeof(a));
	set_text(IDC_X_ELAPSED, a);
	long long total = done_bytes + (st->running ? st->bytes : 0);
	double rate = el > 500 ? total * 1000.0 / el : 0;
	if (bps)
		_snprintf(a, sizeof(a), "%.0f bps", rate * 10);
	else
		_snprintf(a, sizeof(a), "%.0f cps", rate);
	a[sizeof(a) - 1] = '\0';
	set_text(IDC_X_RATE, el > 500 ? a : "");
	if (st->size > 0 && rate > 0 && st->bytes <= st->size) {
		clock_text((DWORD)((st->size - st->bytes) / rate * 1000), a, sizeof(a));
		set_text(IDC_X_LEFT, a);
	} else {
		set_text(IDC_X_LEFT, "");
	}
	size_text(st->bytes, b, sizeof(b));
	if (st->size > 0) {
		size_text(st->size, c, sizeof(c));
		int pct = (int)(st->bytes * 100 / st->size);
		_snprintf(a, sizeof(a), "%s of %s (%d%%)", b, c, pct > 100 ? 100 : pct);
		if (bar)
			SendMessage(bar, PBM_SETPOS, (WPARAM)(pct > 100 ? 100 : pct), 0);
	} else {
		_snprintf(a, sizeof(a), "%s", b);
	}
	a[sizeof(a) - 1] = '\0';
	set_text(IDC_X_BYTES, a);
}

static INT_PTR CALLBACK xfer_proc(HWND d, UINT m, WPARAM wp, LPARAM lp)
{
	(void)lp;
	switch (m) {
	case WM_INITDIALOG:
		return TRUE;
	case WM_COMMAND:
		switch (LOWORD(wp)) {
		case IDC_X_SKIP:
			if (cur) {
				dmc_xfer_skip(cur);
				set_text(IDC_X_RESULT, "Skipping the file...");
			}
			return TRUE;
		case IDCANCEL:
			if (cur) {
				dmc_xfer_cancel(cur);
				set_text(IDC_X_RESULT, "Cancelling...");
				xfer_pump();
			} else {
				DestroyWindow(d);
			}
			return TRUE;
		case IDC_X_UNITS:
			bps = !bps;
			if (cur) {
				dmc_xfer_status st;
				dmc_xfer_get_status(cur, &st);
				show_status(&st, true);
			}
			return TRUE;
		}
		break;
	case WM_DESTROY:
		dlg = bar = NULL;
		break;
	}
	return FALSE;
}

bool xfer_dialog_key(MSG *m)
{
	return dlg && IsDialogMessage(dlg, m);
}

static void open_dialog(int protocol, bool sending, bool resume)
{
	if (dlg)
		DestroyWindow(dlg);
	dlg = CreateDialog(app.inst, MAKEINTRESOURCE(IDD_XFER), app.main, xfer_proc);
	if (!dlg)
		return;
	char a[MAX_PATH + 32], label[80];
	_snprintf(label, sizeof(label), "%s%s", dmc_xfer_label(protocol),
		  resume ? " (crash recovery)" : "");
	label[sizeof(label) - 1] = '\0';
	_snprintf(a, sizeof(a), "%s File - %s", sending ? "Send" : "Receive", label);
	a[sizeof(a) - 1] = '\0';
	SetWindowText(dlg, a);
	set_text(IDC_X_VERB, sending ? "Sending:" : "Receiving:");
	set_text(IDC_X_WHERE, where);
	set_text(IDC_X_PROTO, label);
	/* (Skip file: a receive of the protocols that can tell the sender) */
	EnableWindow(GetDlgItem(dlg, IDC_X_SKIP), !sending && dmc_xfer_can_skip(protocol));
	HWND place = GetDlgItem(dlg, IDC_X_BAR);
	RECT r;
	GetWindowRect(place, &r);
	MapWindowPoints(NULL, dlg, (POINT *)&r, 2);
	bar = CreateWindowEx(0, PROGRESS_CLASS, "", WS_CHILD | WS_VISIBLE, r.left,
			     r.top, r.right - r.left, r.bottom - r.top, dlg, NULL,
			     app.inst, NULL);
	if (bar) {
		ShowWindow(place, SW_HIDE);
		SendMessage(bar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
	}
	ShowWindow(dlg, SW_SHOW);
}

/* ------------------------------------------------------------------ */
/* starting, running, ending */

static void notified(void *ctx)
{
	(void)ctx;
	/* (from the transfer's thread: PostMessage is safe there) */
	PostMessage(app.main, WM_APP_XFER, 0, 0);
}

static bool start(XferLine *l, dmc_xfer *x, int protocol, bool sending, int files,
		  bool resume)
{
	if (!x) {
		MessageBox(app.main, "The transfer could not start.", "File Transfer",
			   MB_ICONWARNING);
		return false;
	}
	dmc_xfer_status st;
	dmc_xfer_get_status(x, &st);
	if (!st.running && st.result[0]) {
		char a[300], m[400];
		to_ansi(st.result, a, sizeof(a));
		_snprintf(m, sizeof(m), "The transfer could not start: %s.", a);
		m[sizeof(m) - 1] = '\0';
		MessageBox(app.main, m, "File Transfer", MB_ICONWARNING);
		dmc_xfer_free(x);
		return false;
	}
	cur = x;
	cur_line = l;
	started_at = last_shown = GetTickCount();
	done_bytes = 0;
	last_files = 0;
	total_files = files;
	open_dialog(protocol, sending, resume);
	dmc_xfer_set_notify(x, notified, NULL);
	xfer_pump();
	return true;
}

static void ended()
{
	dmc_xfer_status st;
	dmc_xfer_get_status(cur, &st);
	show_status(&st, true);
	char rest[4096];
	int n = dmc_xfer_leftover(cur, rest, sizeof(rest));
	char msg[400], a[300];
	const char *what = dmc_xfer_label(st.protocol);
	if (st.result[0]) {
		to_ansi(st.result, a, sizeof(a));
		_snprintf(msg, sizeof(msg), "%s stopped: %s", what, a);
	} else if (st.sending) {
		_snprintf(msg, sizeof(msg), "%s: sent %d file(s)", what, st.files);
	} else {
		_snprintf(msg, sizeof(msg), "%s: received %d file(s) in %s", what,
			  st.files, where);
	}
	msg[sizeof(msg) - 1] = '\0';
	XferLine *l = cur_line;
	dmc_xfer_free(cur);
	cur = NULL;
	cur_line = NULL;
	if (l && *l->view) {
		char line[500];
		_snprintf(line, sizeof(line), "\r\n[%s]\r\n", msg);
		line[sizeof(line) - 1] = '\0';
		termview_feed(*l->view, line, (int)strlen(line));
		if (n > 0)
			termview_feed(*l->view, rest, n);
	}
	app_message(msg);
	if (dlg) {
		if (!st.result[0]) {
			DestroyWindow(dlg);
		} else {
			set_text(IDC_X_RESULT, msg);
			set_text(IDCANCEL, "Close");
			EnableWindow(GetDlgItem(dlg, IDC_X_UNITS), FALSE);
		}
	}
}

void xfer_pump()
{
	if (!cur)
		return;
	if (!cur_line->open())
		dmc_xfer_cancel(cur);
	char buf[4096];
	int room, n;
	while ((room = cur_line->room()) > 0 &&
	       (n = dmc_xfer_output(cur, buf, room < (int)sizeof(buf) ?
				    room : (int)sizeof(buf))) > 0)
		cur_line->write(buf, n);
	dmc_xfer_status st;
	dmc_xfer_get_status(cur, &st);
	if (st.running) {
		show_status(&st, false);
		return;
	}
	/* (the last bytes, ZMODEM's "OO" or a cancel's CANs, sent first) */
	if (cur_line->open() && (n = dmc_xfer_output(cur, buf, sizeof(buf))) > 0) {
		cur_line->write(buf, n);
		return;
	}
	ended();
}

void xfer_cancel()
{
	if (cur) {
		dmc_xfer_cancel(cur);
		xfer_pump();
	}
}

/* ------------------------------------------------------------------ */
/* the remote's own starts */

static int receive_protocol();

static bool start_receive(XferLine *l, int protocol, const char *name, bool resume)
{
	folder(where, sizeof(where));
	dmc_xfer_options o;
	memset(&o, 0, sizeof(o));
	o.resume = resume && protocol == DMC_XFER_ZMODEM;
	o.no_zstd = !xfer_zstd();
	return start(l, dmc_xfer_receive_opt(protocol, where, name, &o), protocol,
		     false, 0, o.resume != 0);
}

bool xfer_take(XferLine *l, const char *data, int n)
{
	if (cur && cur_line == l) {
		dmc_xfer_feed(cur, data, n);
		return true;
	}
	if (cur || n < 4 || !xfer_auto())
		return false;
	int at = 0;
	int d = dmc_xfer_detect(data, n, &at);
	if (d == DMC_XFER_DETECT_ZMODEM_SEND || d == DMC_XFER_DETECT_KERMIT_SEND) {
		int p = d == DMC_XFER_DETECT_KERMIT_SEND ? DMC_XFER_KERMIT :
			DMC_XFER_ZMODEM;
		if (at > 0 && *l->view)
			termview_feed(*l->view, data, at);
		/* (crash recovery as the Receive File dialog last had it) */
		if (start_receive(l, p, NULL, p == receive_protocol() &&
				  ini_int("receive_resume", 0)))
			dmc_xfer_feed(cur, data + at, n - at);
		else if (*l->view)
			termview_feed(*l->view, data + at, n - at);
		return true;
	}
	if (d == DMC_XFER_DETECT_ZMODEM_RECEIVE && !send_wanted) {
		/* (a dialog can't open here, in the middle of the line's read) */
		send_wanted = true;
		PostMessage(app.main, WM_APP_XFER, 1, (LPARAM)l);
	}
	return false;
}

void xfer_message(WPARAM w, LPARAM l)
{
	if (w == 1) {
		XferLine *line = (XferLine *)l;
		if (!cur)
			xfer_send_dialog(line, -2);
		send_wanted = false;
		return;
	}
	xfer_pump();
}

/* ------------------------------------------------------------------ */
/* the Send File and Receive File dialogs */

struct XferAsk {
	XferLine *line;
	bool sending;
	int protocol;
	bool resume;                    /* ZMODEM with Crash Recovery */
	bool remote_ready;              /* its rz waits already: nothing to type */
	char files[MAX_FILES][MAX_PATH];
	int nfiles;
	char command[256], name[MAX_PATH];
};

static int receive_protocol()
{
	int p = ini_int("receive", DMC_XFER_ZMODEM);
	return p >= 0 && p < DMC_XFER_PROTOCOLS ? p : DMC_XFER_ZMODEM;
}

static void protocols_fill(HWND combo, int sel)
{
	SendMessage(combo, CB_RESETCONTENT, 0, 0);
	for (int i = 0; i < CHOICES; i++) {
		char a[80];
		choice_label(i, a, sizeof(a));
		SendMessage(combo, CB_ADDSTRING, 0, (LPARAM)a);
	}
	SendMessage(combo, CB_SETCURSEL, (WPARAM)sel, 0);
}

/* the words under the protocol, and the command it types */
static void protocol_changed(HWND d, XferAsk *a)
{
	int i = (int)SendDlgItemMessage(d, IDC_F_PROTO, CB_GETCURSEL, 0, 0);
	if (i < 0 || i >= CHOICES)
		i = 0;
	int p = choices[i].protocol;
	a->protocol = p;
	a->resume = choices[i].resume != 0;
	SetDlgItemText(d, IDC_F_COMMAND, a->remote_ready ? "" :
		       dmc_xfer_command(p, !a->sending));
	const char *hint;
	if (p == DMC_XFER_TEXT)
		hint = a->sending ?
			"The bytes of the file go as they are, as if typed: to an "
			"editor, a BBS's message editor, or cat > a file (end that "
			"with Ctrl+D)." :
			"What comes is kept as it is, until the line is quiet for "
			"10 seconds (or Cancel: what came is kept).";
	else if (p == DMC_XFER_INTEL_HEX || p == DMC_XFER_SREC)
		hint = a->sending ?
			"The file goes as records, one on each line (a file of "
			"records goes as it is), for an EPROM programmer, a monitor "
			"or a boot loader." :
			"The records that come (their checksums examined) are kept "
			"as the binary image that they hold, gaps as 0xFF.";
	else if (a->resume)
		hint = a->sending ?
			"The receiver is asked to continue a part of a file that it "
			"has (rz -r does). Empty: the remote is already receiving." :
			"A part of a file in the folder is continued from its end, and "
			"a part received is kept if the transfer stops.";
	else if (p == DMC_XFER_YMODEM_G)
		hint = a->sending ?
			"Blocks go with no wait for an ACK, for a line that corrects "
			"its errors (V.42). A receiver that asks for plain YMODEM gets it." :
			"Blocks come with no ACKs, for a line that corrects its errors "
			"(V.42). An error stops the transfer.";
	else if (a->sending)
		hint = "Empty: the remote is already receiving.";
	else
		hint = "Add the names of the files to the command. Empty: start "
			"the remote's end yourself. A remote sz or kermit -s starts a "
			"receive by itself.";
	SetDlgItemText(d, IDC_F_HINT, hint);
	if (!a->sending) {
		bool named = !dmc_xfer_batch(p);
		EnableWindow(GetDlgItem(d, IDC_F_NAME), named);
		EnableWindow(GetDlgItem(d, IDC_F_NAMELABEL), named);
	}
}

/* the files chosen (more than one for a batch protocol) into the box */
static void browse_files(HWND d, XferAsk *a)
{
	static char buf[MAX_FILES * MAX_PATH];
	buf[0] = '\0';
	OPENFILENAME o;
	memset(&o, 0, sizeof(o));
	o.lStructSize = sizeof(o);
	o.hwndOwner = d;
	o.lpstrFilter = "All files (*.*)\0*.*\0";
	o.lpstrFile = buf;
	o.nMaxFile = sizeof(buf);
	o.lpstrTitle = "Send File";
	o.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
	if (dmc_xfer_batch(a->protocol))
		o.Flags |= OFN_ALLOWMULTISELECT;
	if (!GetOpenFileName(&o))
		return;
	/* (more than one: the folder, then each name, each ended by '\0') */
	char text[sizeof(buf) + MAX_FILES * 2];
	const char *next = buf + strlen(buf) + 1;
	if (!*next) {
		lstrcpyn(text, buf, sizeof(text));
	} else {
		text[0] = '\0';
		for (const char *p = next; *p; p += strlen(p) + 1) {
			char one[MAX_PATH * 2];
			_snprintf(one, sizeof(one), "%s%s%s" DIR_SEP "%s", text[0] ? "; " : "",
				  "", buf, p);
			one[sizeof(one) - 1] = '\0';
			if (strlen(text) + strlen(one) < sizeof(text))
				strcat(text, one);
		}
	}
	SetDlgItemText(d, IDC_F_FILE, text);
}

static void browse_folder(HWND d)
{
#ifndef DMODEM_WINPORT
	char path[MAX_PATH];
	BROWSEINFO b;
	memset(&b, 0, sizeof(b));
	b.hwndOwner = d;
	b.pszDisplayName = path;
	b.lpszTitle = "Place received files in this folder:";
	b.ulFlags = BIF_RETURNONLYFSDIRS;
	LPITEMIDLIST id = SHBrowseForFolder(&b);
	if (!id)
		return;
	if (SHGetPathFromIDList(id, path))
		SetDlgItemText(d, IDC_F_FILE, path);
	IMalloc *m;
	if (SUCCEEDED(SHGetMalloc(&m))) {
		m->Free(id);
		m->Release();
	}
#else
	(void)d;
#endif
}

/* the box's files, split at ';' (quotes taken off) */
static int split_files(const char *text, XferAsk *a)
{
	a->nfiles = 0;
	const char *p = text;
	while (*p && a->nfiles < MAX_FILES) {
		while (*p == ' ' || *p == ';' || *p == '"')
			p++;
		const char *e = p;
		while (*e && *e != ';')
			e++;
		const char *t = e;
		while (t > p && (t[-1] == ' ' || t[-1] == '"'))
			t--;
		if (t > p) {
			int n = (int)(t - p) < MAX_PATH - 1 ? (int)(t - p) : MAX_PATH - 1;
			memcpy(a->files[a->nfiles], p, (size_t)n);
			a->files[a->nfiles][n] = '\0';
			a->nfiles++;
		}
		p = e;
	}
	return a->nfiles;
}

static INT_PTR CALLBACK ask_proc(HWND d, UINT m, WPARAM wp, LPARAM lp)
{
	XferAsk *a = (XferAsk *)GetWindowLongPtr(d, DWLP_USER);
	switch (m) {
	case WM_INITDIALOG: {
		a = (XferAsk *)lp;
		SetWindowLongPtr(d, DWLP_USER, (LONG_PTR)a);
		protocols_fill(GetDlgItem(d, IDC_F_PROTO), choice_of(a->protocol, a->resume));
		SendDlgItemMessage(d, IDC_F_COMMAND, EM_LIMITTEXT, sizeof(a->command) - 1, 0);
		if (a->sending) {
			char path[MAX_PATH], v[MAX_PATH];
			ini(path, sizeof(path));
			GetPrivateProfileString("transfer", "file", "", v, sizeof(v), path);
			SetDlgItemText(d, IDC_F_FILE, v);
		} else {
			char f[MAX_PATH];
			folder(f, sizeof(f));
			SetDlgItemText(d, IDC_F_FILE, f);
#ifdef DMODEM_WINPORT
			ShowWindow(GetDlgItem(d, IDC_F_BROWSE), SW_HIDE);
#endif
		}
		protocol_changed(d, a);
		return TRUE;
	}
	case WM_COMMAND:
		switch (LOWORD(wp)) {
		case IDC_F_PROTO:
			if (HIWORD(wp) == CBN_SELCHANGE)
				protocol_changed(d, a);
			return TRUE;
		case IDC_F_BROWSE:
			if (a->sending)
				browse_files(d, a);
			else
				browse_folder(d);
			return TRUE;
		case IDOK: {
			char text[MAX_FILES * MAX_PATH];
			GetDlgItemText(d, IDC_F_FILE, text, sizeof(text));
			GetDlgItemText(d, IDC_F_COMMAND, a->command, sizeof(a->command));
			if (a->sending) {
				if (!split_files(text, a)) {
					MessageBox(d, "Type the name of a file, or use Browse.",
						   "Send File", MB_ICONINFORMATION);
					return TRUE;
				}
				if (!dmc_xfer_batch(a->protocol))
					a->nfiles = 1;
				ini_set("file", a->files[0]);
				ini_set_int("send", a->protocol);
				ini_set_int("send_resume", a->resume);
			} else {
				if (!text[0]) {
					MessageBox(d, "Type the folder for the received files.",
						   "Receive File", MB_ICONINFORMATION);
					return TRUE;
				}
				CreateDirectory(text, NULL);
				ini_set("folder", text);
				ini_set_int("receive", a->protocol);
				ini_set_int("receive_resume", a->resume);
				GetDlgItemText(d, IDC_F_NAME, a->name, sizeof(a->name));
			}
			EndDialog(d, IDOK);
			return TRUE;
		}
		case IDCANCEL:
			EndDialog(d, IDCANCEL);
			return TRUE;
		}
		break;
	}
	return FALSE;
}

static bool line_ready(XferLine *l)
{
	if (cur) {
		MessageBox(app.main, "A file transfer runs now.", "File Transfer",
			   MB_ICONINFORMATION);
		return false;
	}
	if (!l->open()) {
		char m[200];
		_snprintf(m, sizeof(m), "There is no connection to %s.", l->what);
		m[sizeof(m) - 1] = '\0';
		MessageBox(app.main, m, "File Transfer", MB_ICONINFORMATION);
		return false;
	}
	return true;
}

/* protocol -2: the remote's rz waits (ZMODEM, nothing to type) */
void xfer_send_dialog(XferLine *l, int protocol)
{
	if (!line_ready(l))
		return;
	static XferAsk a;
	memset(&a, 0, sizeof(a));
	a.line = l;
	a.sending = true;
	a.remote_ready = protocol == -2;
	a.protocol = protocol >= 0 ? protocol : protocol == -2 ? DMC_XFER_ZMODEM :
		ini_int("send", DMC_XFER_ZMODEM);
	if (a.protocol < 0 || a.protocol >= DMC_XFER_PROTOCOLS)
		a.protocol = DMC_XFER_ZMODEM;
	a.resume = protocol == -1 && a.protocol == DMC_XFER_ZMODEM &&
		ini_int("send_resume", 0);
	if (DialogBoxParam(app.inst, MAKEINTRESOURCE(IDD_XSEND), app.main, ask_proc,
			   (LPARAM)&a) != IDOK || !line_ready(l))
		return;
	const char *paths[MAX_FILES];
	for (int i = 0; i < a.nfiles; i++)
		paths[i] = a.files[i];
	/* the remote's receive started first (its rz waits for us anyway) */
	if (a.command[0]) {
		char c[300];
		_snprintf(c, sizeof(c), "%s\r", a.command);
		c[sizeof(c) - 1] = '\0';
		l->write(c, (int)strlen(c));
	}
	_snprintf(where, sizeof(where), "%s", a.files[0]);
	where[sizeof(where) - 1] = '\0';
	char *slash = strrchr(where, DIR_SEP[0]);
	if (slash)
		*slash = '\0';
	/* (plain text paced as ASCII Setup has it) */
	dmc_xfer_options o;
	memset(&o, 0, sizeof(o));
	o.resume = a.resume;
	o.no_zstd = !xfer_zstd();
	o.line_delay_ms = ascii_int("line_delay", 0);
	o.char_delay_ms = ascii_int("char_delay", 0);
	start(l, dmc_xfer_send_opt(a.protocol, paths, a.nfiles, &o), a.protocol, true,
	      a.nfiles, a.resume);
}

void xfer_receive_dialog(XferLine *l)
{
	if (!line_ready(l))
		return;
	static XferAsk a;
	memset(&a, 0, sizeof(a));
	a.line = l;
	a.sending = false;
	a.protocol = receive_protocol();
	a.resume = a.protocol == DMC_XFER_ZMODEM && ini_int("receive_resume", 0);
	if (DialogBoxParam(app.inst, MAKEINTRESOURCE(IDD_XRECV), app.main, ask_proc,
			   (LPARAM)&a) != IDOK || !line_ready(l))
		return;
	/* (the protocols that send no name: the name typed, else the last
	   word of the command) */
	char name[MAX_PATH];
	_snprintf(name, sizeof(name), "%s", a.name);
	name[sizeof(name) - 1] = '\0';
	if (!name[0]) {
		const char *w = strrchr(a.command, ' ');
		w = w ? w + 1 : a.command;
		const char *s = strrchr(w, '/');
		_snprintf(name, sizeof(name), "%s", s ? s + 1 : w);
		name[sizeof(name) - 1] = '\0';
	}
	/* the remote's send started first (its sz or sx waits for us), unless
	   the command still wants its files' names */
	int cl = (int)strlen(a.command);
	if (cl && a.command[cl - 1] != ' ') {
		char c[300];
		_snprintf(c, sizeof(c), "%s\r", a.command);
		c[sizeof(c) - 1] = '\0';
		l->write(c, (int)strlen(c));
	}
	start_receive(l, a.protocol, name[0] ? name : NULL, a.resume);
}
