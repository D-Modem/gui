/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * remote.cpp - the Remote page: a login to a server by SSH, Telnet or
 *              rlogin, in a terminal of its own (termview.cpp)
 *
 * Telnet and rlogin: a Winsock 1.1 socket (WSAAsyncSelect, as the
 * modem's link), the protocol libdmodemctl's dmc_net; every Windows from
 * 95 on has them.  SSH: libdmodemssh (the apps' own client, on libssh),
 * on a thread of its own that posts its events to the main window; it
 * reads OpenSSH's config, known_hosts and keys in the profile's .ssh,
 * and asks Pageant (or else OpenSSH's own agent) for its keys; or logs
 * in by the agent alone, or by a key file: OpenSSH's, PEM or PuTTY's
 * .ppk, which can also be imported into .ssh in OpenSSH's form.
 * The Windows 95 build (Open Watcom, no libssh) runs Windows' own
 * ssh.exe in a pseudo console (ConPTY) instead, when it finds one
 * (Windows 10 1809 and later): the ConPTY calls are looked up at run
 * time and its output read from a timer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "resource.h"

#ifdef DMODEM_HAVE_SSH
#include <commdlg.h>
#endif

static HWND page, view, type_box, proto_box, host_ed, port_ed, user_ed,
	go_btn, status_txt, login_btn;

/* the protocols in the list's order */
static const int protos[] = { DMC_NET_SSH, DMC_NET_TELNET, DMC_NET_RLOGIN };
#define NPROTOS 3

/* what runs: its protocol (0: none) */
static int running;
static int cols, rows;

/* Telnet, rlogin */
static SOCKET sock = INVALID_SOCKET;
static dmc_net *net;
static bool sock_open;
static char *pending;
static int pending_n, pending_cap;

#ifdef DMODEM_HAVE_SSH
#include "dmodemssh.h"

/* SSH: the connection (below) */
struct Ssh;
static Ssh *ssh;
static int shell;
static dmc_ssh *ssh_conn();
static void ssh_disconnect();
#else
/* SSH: the pseudo console, its pipes, ssh.exe */
static void *hpc;
static HANDLE con_in = INVALID_HANDLE_VALUE, con_out = INVALID_HANDLE_VALUE;
static HANDLE proc;

/*
 * ConPTY, looked up at run time (Windows 10 1809 and later)
 */

#ifndef EXTENDED_STARTUPINFO_PRESENT
#define EXTENDED_STARTUPINFO_PRESENT 0x00080000
#endif
#define ATTR_PSEUDOCONSOLE 0x00020016

typedef long (WINAPI *CreatePCFn)(COORD, HANDLE, HANDLE, DWORD, void **);
typedef long (WINAPI *ResizePCFn)(void *, COORD);
typedef void (WINAPI *ClosePCFn)(void *);
typedef BOOL (WINAPI *InitAttrsFn)(void *, DWORD, DWORD, size_t *);
typedef BOOL (WINAPI *UpdateAttrFn)(void *, DWORD, size_t, void *, size_t,
				    void *, size_t *);
typedef void (WINAPI *DeleteAttrsFn)(void *);

static CreatePCFn create_pc;
static ResizePCFn resize_pc;
static ClosePCFn close_pc;
static InitAttrsFn init_attrs;
static UpdateAttrFn update_attr;
static DeleteAttrsFn delete_attrs;

/* STARTUPINFOEX, for headers without it */
struct StartupInfoEx {
	STARTUPINFOA si;
	void *attrs;
};

/* (through a plain function pointer: the cast that compilers take) */
#define LOOKUP(k, name) (void (*)(void))GetProcAddress(k, name)

static bool conpty_available()
{
	HMODULE k = GetModuleHandle("kernel32.dll");
	if (!k)
		return false;
	create_pc = (CreatePCFn)LOOKUP(k, "CreatePseudoConsole");
	resize_pc = (ResizePCFn)LOOKUP(k, "ResizePseudoConsole");
	close_pc = (ClosePCFn)LOOKUP(k, "ClosePseudoConsole");
	init_attrs = (InitAttrsFn)LOOKUP(k, "InitializeProcThreadAttributeList");
	update_attr = (UpdateAttrFn)LOOKUP(k, "UpdateProcThreadAttribute");
	delete_attrs = (DeleteAttrsFn)LOOKUP(k, "DeleteProcThreadAttributeList");
	return create_pc && resize_pc && close_pc && init_attrs &&
		update_attr && delete_attrs;
}

static bool exists(const char *p)
{
	DWORD a = GetFileAttributes(p);
	return a != 0xFFFFFFFF && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* ssh.exe: on the PATH, or Windows' own (Sysnative: the 64-bit System32
   seen from this 32-bit app) */
static bool find_ssh(char *out, int n)
{
	char *part;
	if (SearchPath(NULL, "ssh.exe", NULL, (DWORD)n, out, &part))
		return true;
	char root[MAX_PATH];
	if (!GetEnvironmentVariable("SystemRoot", root, sizeof(root)))
		return false;
	static const char *const dirs[] = { "Sysnative", "System32" };
	for (int i = 0; i < 2; i++) {
		_snprintf(out, n, "%s\\%s\\OpenSSH\\ssh.exe", root, dirs[i]);
		out[n - 1] = '\0';
		if (exists(out))
			return true;
	}
	return false;
}
#endif

/*
 * the page
 */

static void set_status(const char *s)
{
	char a[300];
	to_ansi(s, a, sizeof(a));
	SetWindowText(status_txt, a);
}

static void controls()
{
	bool idle = !running;
	EnableWindow(proto_box, idle);
	EnableWindow(host_ed, idle);
	EnableWindow(port_ed, idle);
	EnableWindow(user_ed, idle);
	SetWindowText(go_btn, idle ? "&Connect" : "&Disconnect");
	if (login_btn) {
		int i = (int)SendMessage(proto_box, CB_GETCURSEL, 0, 0);
		EnableWindow(login_btn, idle && (i < 0 || protos[i] == DMC_NET_SSH));
	}
}

static void show(const char *s, int n)
{
	termview_feed(view, s, n);
}

static void show_line(const char *s)
{
	show(s, (int)strlen(s));
}

static void ended(const char *why)
{
	if (!running)
		return;
	running = 0;
	KillTimer(app.main, TIMER_REMOTE);
	if (sock != INVALID_SOCKET) {
		WSAAsyncSelect(sock, app.main, 0, 0);
		closesocket(sock);
		sock = INVALID_SOCKET;
	}
	sock_open = false;
	if (net) {
		dmc_net_free(net);
		net = NULL;
	}
	pending_n = 0;
#ifdef DMODEM_HAVE_SSH
	/* (the connection is freed once its thread has said it's closed) */
	if (ssh)
		ssh_disconnect();
	shell = 0;
#else
	if (proc) {
		TerminateProcess(proc, 0);
		CloseHandle(proc);
		proc = NULL;
	}
	/* (our end of its output shut first, so that closing the console
	   can't wait on a full pipe) */
	if (con_out != INVALID_HANDLE_VALUE) {
		CloseHandle(con_out);
		con_out = INVALID_HANDLE_VALUE;
	}
	if (hpc) {
		close_pc(hpc);
		hpc = NULL;
	}
	if (con_in != INVALID_HANDLE_VALUE) {
		CloseHandle(con_in);
		con_in = INVALID_HANDLE_VALUE;
	}
#endif
	char line[300];
	_snprintf(line, sizeof(line), "\r\n\x1b[0m[%s]\r\n",
		  why ? why : "Disconnected");
	line[sizeof(line) - 1] = '\0';
	show_line(line);
	set_status(why ? why : "Disconnected");
	controls();
}

/* Telnet and rlogin: the protocol's output written, as much as goes */
static void flush()
{
	char buf[2048];
	int n;
	while (net && (n = dmc_net_output(net, buf, sizeof(buf))) > 0) {
		if (pending_n + n > pending_cap) {
			pending_cap = (pending_n + n) * 2;
			pending = (char *)realloc(pending, (size_t)pending_cap);
		}
		memcpy(pending + pending_n, buf, (size_t)n);
		pending_n += n;
	}
	while (sock_open && pending_n > 0) {
		int w = send(sock, pending, pending_n, 0);
		if (w == SOCKET_ERROR) {
			if (WSAGetLastError() == WSAEWOULDBLOCK)
				return;         /* FD_WRITE: more */
			ended("The connection failed");
			return;
		}
		memmove(pending, pending + w, (size_t)(pending_n - w));
		pending_n -= w;
	}
}

static void pump()
{
	char buf[4096];
	int n;
	while (net && (n = dmc_net_screen(net, buf, sizeof(buf))) > 0)
		show(buf, n);
	flush();
}

/* what the terminal gives: to the far end */
static void typed(const char *data, int n, void *ctx)
{
	(void)ctx;
	if (!running)
		return;
	if (net && sock_open) {
		dmc_net_type(net, data, n);
		flush();
	}
#ifdef DMODEM_HAVE_SSH
	else if (ssh && shell > 0)
		dmc_ssh_write(ssh_conn(), shell, data, n);
#else
	else if (con_in != INVALID_HANDLE_VALUE) {
		DWORD w;
		WriteFile(con_in, data, (DWORD)n, &w, NULL);
	}
#endif
}

void remote_socket(WPARAM wp, LPARAM lp)
{
	if ((SOCKET)wp != sock || !net)
		return;
	int err = WSAGETSELECTERROR(lp);
	switch (WSAGETSELECTEVENT(lp)) {
	case FD_CONNECT:
		if (err) {
			char m[160];
			_snprintf(m, sizeof(m), "Could not connect (error %d)", err);
			m[sizeof(m) - 1] = '\0';
			ended(m);
			return;
		}
		sock_open = true;
		set_status("Connected");
		dmc_net_connected(net);
		pump();
		break;
	case FD_OOB: {
		char b;
		if (recv(sock, &b, 1, MSG_OOB) == 1)
			dmc_net_urgent(net, (unsigned char)b);
		pump();
		break;
	}
	case FD_READ: {
		char buf[4096];
		int n = recv(sock, buf, sizeof(buf), 0);
		if (n > 0)
			dmc_net_feed(net, buf, n);
		pump();
		break;
	}
	case FD_WRITE:
		flush();
		break;
	case FD_CLOSE: {
		char buf[4096];
		int n;
		while ((n = recv(sock, buf, sizeof(buf), 0)) > 0)
			dmc_net_feed(net, buf, n);
		pump();
		ended("Connection closed by the server");
		break;
	}
	}
}

/* the timer: ssh's output, its end, and the terminal's size */
void remote_tick()
{
	if (!running)
		return;
	int c, r;
	termview_size(view, &c, &r);
	if (c != cols || r != rows) {
		cols = c;
		rows = r;
		if (net) {
			dmc_net_resize(net, c, r);
			flush();
		}
#ifdef DMODEM_HAVE_SSH
		else if (ssh && shell > 0)
			dmc_ssh_resize(ssh_conn(), shell, c, r);
#else
		else if (hpc) {
			COORD sz;
			sz.X = (SHORT)c;
			sz.Y = (SHORT)r;
			resize_pc(hpc, sz);
		}
#endif
	}
#ifndef DMODEM_HAVE_SSH
	if (con_out == INVALID_HANDLE_VALUE)
		return;
	for (;;) {
		DWORD avail = 0;
		if (!PeekNamedPipe(con_out, NULL, 0, NULL, &avail, NULL) ||
		    !avail)
			break;
		char buf[4096];
		DWORD n = 0;
		if (!ReadFile(con_out, buf, avail < sizeof(buf) ? avail :
			      sizeof(buf), &n, NULL) || !n)
			break;
		show(buf, (int)n);
	}
	if (proc && WaitForSingleObject(proc, 0) == WAIT_OBJECT_0) {
		DWORD code = 0;
		GetExitCodeProcess(proc, &code);
		char m[80];
		if (code)
			_snprintf(m, sizeof(m), "ssh exited with status %lu",
				  (unsigned long)code);
		else
			_snprintf(m, sizeof(m), "Connection closed");
		m[sizeof(m) - 1] = '\0';
		ended(m);
	}
#endif
}

#ifdef DMODEM_HAVE_SSH
/*
 * SSH: libdmodemssh, polled on a thread of its own, its events posted
 * to the main window (WM_APP_SSH)
 */

/* a connection and the thread that polls it */
struct Ssh {
	dmc_ssh *s;
	HANDLE thread;
};

/* one of its events, posted */
struct SshMsg {
	Ssh *ssh;
	int type, channel, value, len;
	char *data;
};

static int tried_keys, asks, exit_status;

static void ini(char *out, int n);

/*
 * How it logs in: [remote] login ("auto", "agent", "key"), key (a file)
 * and agent ("": Pageant, else OpenSSH's), in dmodem.ini
 */

struct Login {
	char how[16], key[MAX_PATH], agent[MAX_PATH];
};

static void login_read(Login *l)
{
	char path[MAX_PATH];
	ini(path, sizeof(path));
	GetPrivateProfileString("remote", "login", "auto", l->how, sizeof(l->how), path);
	GetPrivateProfileString("remote", "key", "", l->key, sizeof(l->key), path);
	GetPrivateProfileString("remote", "agent", "", l->agent, sizeof(l->agent), path);
}

static void login_write(const Login *l)
{
	char path[MAX_PATH];
	ini(path, sizeof(path));
	WritePrivateProfileString("remote", "login", l->how, path);
	WritePrivateProfileString("remote", "key", l->key, path);
	WritePrivateProfileString("remote", "agent", l->agent, path);
}

/* a file's text (malloc'd, wiped and freed by text_free), or NULL */
static char *text_read(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	char *t = (char *)malloc(65536);
	size_t n = t ? fread(t, 1, 65535, f) : 0;
	fclose(f);
	if (t)
		t[n] = '\0';
	return t;
}

static void text_free(char *t)
{
	if (t) {
		SecureZeroMemory(t, strlen(t));
		free(t);
	}
}

/* the key file's text, while its login goes on; passphrases tried */
static char *key_text;
static int tried_file, key_tries;

/* the connection's dmc_ssh */
static dmc_ssh *ssh_conn()
{
	return ssh->s;
}

/* the connection closed by the user: its thread ends with it */
static void ssh_disconnect()
{
	dmc_ssh_disconnect(ssh->s);
}

static DWORD WINAPI ssh_poll(void *p)
{
	Ssh *c = (Ssh *)p;
	dmc_ssh_event ev;
	int n;
	while ((n = dmc_ssh_poll(c->s, 1000, &ev)) >= 0) {
		if (n == 0)
			continue;
		SshMsg *m = (SshMsg *)calloc(1, sizeof(SshMsg));
		if (!m)
			continue;
		m->ssh = c;
		m->type = ev.type;
		m->channel = ev.channel;
		m->value = ev.value;
		m->len = ev.len;
		if (ev.data) {
			m->data = (char *)malloc((size_t)ev.len + 1);
			if (m->data) {
				memcpy(m->data, ev.data, (size_t)ev.len);
				m->data[ev.len] = '\0';
			}
		}
		/* (a window gone: the event too) */
		if (!PostMessage(app.main, WM_APP_SSH, 0, (LPARAM)m)) {
			free(m->data);
			free(m);
		}
		if (ev.type == DMC_SSH_EV_CLOSED)
			break;
	}
	return 0;
}

/* text the dialog shows (the system's code page) */
static void set_text(HWND d, int id, const char *utf8)
{
	char a[600];
	to_ansi(utf8 ? utf8 : "", a, sizeof(a));
	SetDlgItemText(d, id, a);
}

/* an answer typed (the system's code page) as UTF-8 */
static void get_answer(HWND d, int id, char *out, int n)
{
	char a[512];
	wchar_t w[512];
	GetDlgItemText(d, id, a, sizeof(a));
	int k = MultiByteToWideChar(CP_ACP, 0, a, -1, w, 512);
	if (!k || !WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL))
		lstrcpyn(out, a, n);
}

/* what the login dialog asks: a password (prompts 0) or the server's
   questions (four at most) */
struct Ask {
	const char *head, *text;
	const char *label;	/* the one answer's (prompts 0): "Password:" */
	const char *ok;		/* its button's: "&Log In" */
	int prompts;
	char answers[4][256];
};

static INT_PTR CALLBACK ask_proc(HWND d, UINT msg, WPARAM wp, LPARAM lp)
{
	Ask *a = (Ask *)GetWindowLongPtr(d, DWLP_USER);
	switch (msg) {
	case WM_INITDIALOG: {
		a = (Ask *)lp;
		SetWindowLongPtr(d, DWLP_USER, (LONG_PTR)a);
		set_text(d, IDC_A_HEAD, a->head);
		if (a->ok)
			SetDlgItemText(d, IDOK, a->ok);
		set_text(d, IDC_A_TEXT, a->text);
		int n = a->prompts ? a->prompts : 1;
		HWND first = NULL;
		for (int i = 0; i < 4; i++) {
			int echo = 0;
			const char *q = i >= n ? "" : a->prompts ?
				dmc_ssh_prompt(ssh->s, i, &echo) :
				a->label ? a->label : "Password:";
			set_text(d, IDC_A_Q0 + i, q);
			ShowWindow(GetDlgItem(d, IDC_A_Q0 + i), i < n ? SW_SHOW : SW_HIDE);
			ShowWindow(GetDlgItem(d, IDC_A_E0 + i), i < n && echo ? SW_SHOW : SW_HIDE);
			ShowWindow(GetDlgItem(d, IDC_A_E0P + i), i < n && !echo ? SW_SHOW : SW_HIDE);
			if (i < n && !first)
				first = GetDlgItem(d, echo ? IDC_A_E0 + i : IDC_A_E0P + i);
		}
		/* (the buttons under the last question, the box fitted to them) */
		RECT b = { 0, 46 + 18 * n + 8, 50, 46 + 18 * n + 8 + 14 };
		MapDialogRect(d, &b);
		RECT ok, cl, wr;
		GetWindowRect(GetDlgItem(d, IDOK), &ok);
		MapWindowPoints(NULL, d, (POINT *)&ok, 2);
		SetWindowPos(GetDlgItem(d, IDOK), NULL, ok.left, b.top, 0, 0,
			     SWP_NOSIZE | SWP_NOZORDER);
		GetWindowRect(GetDlgItem(d, IDCANCEL), &ok);
		MapWindowPoints(NULL, d, (POINT *)&ok, 2);
		SetWindowPos(GetDlgItem(d, IDCANCEL), NULL, ok.left, b.top, 0, 0,
			     SWP_NOSIZE | SWP_NOZORDER);
		GetClientRect(d, &cl);
		GetWindowRect(d, &wr);
		RECT pad = { 0, 0, 0, 8 };
		MapDialogRect(d, &pad);
		int extra = (wr.bottom - wr.top) - cl.bottom;
		SetWindowPos(d, NULL, 0, 0, wr.right - wr.left,
			     b.bottom + pad.bottom + extra, SWP_NOMOVE | SWP_NOZORDER);
		SetFocus(first);
		return FALSE;
	}
	case WM_COMMAND:
		if (LOWORD(wp) == IDOK) {
			for (int i = 0; i < 4; i++) {
				int id = IsWindowVisible(GetDlgItem(d, IDC_A_E0 + i)) ?
					IDC_A_E0 + i : IDC_A_E0P + i;
				get_answer(d, id, a->answers[i], sizeof(a->answers[i]));
			}
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

/* the server's key: known goes on; new is asked; changed is refused */
static void ssh_hostkey(const char *key, int known)
{
	char fp[128] = "";
	dmc_ssh_fingerprint(key, fp, sizeof(fp));
	int tl = (int)strcspn(key, " ");
	if (known == DMC_SSH_HOST_KNOWN) {
		dmc_ssh_trust(ssh->s, 1, 0);
		return;
	}
	char m[600];
	if (known == DMC_SSH_HOST_CHANGED || known == DMC_SSH_HOST_OTHER_TYPE) {
		_snprintf(m, sizeof(m),
			  "\r\n\x1b[1;31mThe server's host key has changed (it is now %.*s %s).\r\n"
			  "Someone may be in the middle; or the server has a new key. If that is\r\n"
			  "expected, remove its old line from .ssh\\known_hosts in your profile.\x1b[0m\r\n",
			  tl, key, fp);
		m[sizeof(m) - 1] = '\0';
		show_line(m);
		dmc_ssh_trust(ssh->s, 0, 0);
		return;
	}
	Ssh *c = ssh;
	_snprintf(m, sizeof(m), "This server is not known yet. Its key's fingerprint is\n\n"
		  "%.*s %s\n\nIf it's the one you expect, it will be added to "
		  ".ssh\\known_hosts in your profile.\n\nConnect?", tl, key, fp);
	m[sizeof(m) - 1] = '\0';
	int r = MessageBox(app.main, m, "Connect to a new server?",
			   MB_YESNO | MB_ICONQUESTION);
	/* (the connection may have gone while it asked) */
	if (c == ssh)
		dmc_ssh_trust(ssh->s, r == IDYES, 1);
}

/* a password or the server's questions, asked */
static void ssh_ask(const char *head, const char *text, int prompts)
{
	Ssh *c = ssh;
	Ask a;
	memset(&a, 0, sizeof(a));
	a.head = head;
	a.text = text;
	a.prompts = prompts > 4 ? 4 : prompts;
	INT_PTR r = DialogBoxParam(app.inst, MAKEINTRESOURCE(IDD_ASK), app.main,
				   ask_proc, (LPARAM)&a);
	if (c != ssh)
		return;
	if (r != IDOK)
		dmc_ssh_auth_cancel(ssh->s);
	else if (!prompts)
		dmc_ssh_auth_password(ssh->s, a.answers[0]);
	else {
		for (int i = 0; i < a.prompts; i++)
			dmc_ssh_answer(ssh->s, i, a.answers[i]);
		dmc_ssh_answered(ssh->s);
	}
	SecureZeroMemory(&a, sizeof(a));
}

/* a key's passphrase asked (why: a try's failure, or ""): FALSE when
   cancelled */
static bool ask_passphrase(HWND owner, const char *file, const char *why,
			   const char *ok, char *out, int n)
{
	Ask a;
	memset(&a, 0, sizeof(a));
	const char *name = strrchr(file, '\\');
	char head[MAX_PATH + 32];
	_snprintf(head, sizeof(head), "The passphrase for %s", name ? name + 1 : file);
	head[sizeof(head) - 1] = '\0';
	a.head = head;
	a.text = why;
	a.label = "Passphrase:";
	a.ok = ok;
	INT_PTR r = DialogBoxParam(app.inst, MAKEINTRESOURCE(IDD_ASK), owner,
				   ask_proc, (LPARAM)&a);
	if (r == IDOK)
		lstrcpyn(out, a.answers[0], n);
	SecureZeroMemory(&a, sizeof(a));
	return r == IDOK;
}

/* the login by the key file (its passphrase asked if it has one): FALSE
   when there's nothing to try */
static bool key_login(const char *file)
{
	Ssh *c = ssh;
	text_free(key_text);
	key_text = text_read(file);
	tried_file = 1;
	if (!key_text) {
		char m[MAX_PATH + 64];
		_snprintf(m, sizeof(m), "\r\n[The key file %s can't be read]\r\n", file);
		m[sizeof(m) - 1] = '\0';
		show_line(m);
		return false;
	}
	if (!dmc_ssh_key_encrypted(key_text)) {
		if (dmc_ssh_auth_key(ssh->s, key_text, NULL) == 0)
			return true;
		show_line("\r\n[The key file is not a key that can be read]\r\n");
		return false;
	}
	const char *why = "";
	for (key_tries = 0; key_tries < 3; key_tries++) {
		char pass[256];
		bool ok = ask_passphrase(app.main, file, why, NULL, pass, sizeof(pass));
		/* (the connection may have gone while it asked) */
		if (c != ssh || !key_text) {
			SecureZeroMemory(pass, sizeof(pass));
			return true;
		}
		if (!ok)
			return false;
		int r = dmc_ssh_auth_key(ssh->s, key_text, pass);
		SecureZeroMemory(pass, sizeof(pass));
		if (r == 0)
			return true;
		why = "That passphrase does not open the key.";
	}
	return false;
}

/* what's tried, in OpenSSH's order: its keys and the agent (or what was
   chosen: the agent alone, or a key file), then the server's questions
   or a password (three times at most) */
static void ssh_auth(int methods, const char *why)
{
	Login l;
	login_read(&l);
	bool file = !lstrcmpi(l.how, "key") && l.key[0];
	if ((methods & DMC_SSH_AUTH_PUBLICKEY) && file && !tried_file) {
		if (key_login(l.key))
			return;
	} else if ((methods & DMC_SSH_AUTH_PUBLICKEY) && !file && !tried_keys) {
		tried_keys = 1;
		if (!lstrcmpi(l.how, "agent"))
			dmc_ssh_auth_agent(ssh->s);
		else
			dmc_ssh_auth_auto(ssh->s);
		return;
	}
	text_free(key_text);
	key_text = NULL;
	if (!ssh)
		return;
	if (asks++ >= 3) {
		dmc_ssh_auth_cancel(ssh->s);
		return;
	}
	char host[256], head[300];
	GetWindowText(host_ed, host, sizeof(host));
	_snprintf(head, sizeof(head), "Log in to %s", host);
	head[sizeof(head) - 1] = '\0';
	if (methods & DMC_SSH_AUTH_INTERACTIVE)
		dmc_ssh_auth_interactive(ssh->s);
	else if (methods & DMC_SSH_AUTH_PASSWORD)
		ssh_ask(head, asks > 1 ? why : "", 0);
	else
		dmc_ssh_auth_cancel(ssh->s);
}

void remote_ssh(LPARAM l)
{
	SshMsg *m = (SshMsg *)l;
	Ssh *c = m->ssh;
	if (m->type == DMC_SSH_EV_CLOSED) {
		/* (its thread has ended: the connection can go) */
		WaitForSingleObject(c->thread, INFINITE);
		CloseHandle(c->thread);
		dmc_ssh_free(c->s);
		free(c);
		if (c == ssh) {
			text_free(key_text);
			key_text = NULL;
			ssh = NULL;
			shell = 0;
			if (!m->data && exit_status > 0) {
				char why[64];
				_snprintf(why, sizeof(why), "the shell exited with status %d",
					  exit_status);
				why[sizeof(why) - 1] = '\0';
				ended(why);
			} else {
				ended(m->data);
			}
		}
	} else if (c == ssh) {
		switch (m->type) {
		case DMC_SSH_EV_HOSTKEY:
			ssh_hostkey(m->data, m->value);
			break;
		case DMC_SSH_EV_AUTH:
			ssh_auth(m->value, m->data ? m->data : "");
			break;
		case DMC_SSH_EV_PROMPTS:
			if (m->value == 0)
				dmc_ssh_answered(ssh->s);
			else
				ssh_ask(m->data && *m->data ? m->data : "The server asks",
					dmc_ssh_instruction(ssh->s), m->value);
			break;
		case DMC_SSH_EV_READY:
			if (m->data && *m->data)
				show(m->data, m->len);
			shell = dmc_ssh_shell(ssh->s,
					      dmc_vt_profile_term(termview_profile(view)),
					      cols, rows, 0);
			set_status("Connected by SSH");
			break;
		case DMC_SSH_EV_OPEN:
			if (!m->value) {
				show_line("\r\n[The server refused the shell]\r\n");
				dmc_ssh_disconnect(ssh->s);
			}
			break;
		case DMC_SSH_EV_DATA:
			if (m->channel == shell)
				show(m->data, m->len);
			break;
		case DMC_SSH_EV_EXIT:
			if (m->channel == shell)
				exit_status = m->value;
			break;
		case DMC_SSH_EV_CLOSE:
			/* (the shell's over: so is the connection) */
			if (m->channel == shell)
				dmc_ssh_disconnect(ssh->s);
			break;
		case DMC_SSH_EV_INCOMING:
			dmc_ssh_close(ssh->s, m->channel);
			break;
		}
	}
	free(m->data);
	free(m);
}

static bool start_ssh(const char *host, int port, const char *user)
{
	char me[128];
	DWORD n = sizeof(me);
	if (!*user && GetUserName(me, &n))
		user = me;
	dmc_ssh *s = dmc_ssh_new(host, port, user);
	if (!s) {
		app_message("Could not start SSH");
		return false;
	}
	/* OpenSSH's config and known hosts, in the profile's .ssh */
	dmc_ssh_option(s, "config", "");
	dmc_ssh_option(s, "knownhosts", "");
	/* the agent: the one given, else Pageant, else OpenSSH's */
	Login l;
	login_read(&l);
	if (l.agent[0])
		dmc_ssh_option(s, "agent", l.agent);
	if (!lstrcmpi(l.how, "agent") && !dmc_ssh_agent_reachable(l.agent))
		show_line(l.agent[0] ? "[No agent answers there: is it running?]\r\n" :
			  "[Neither Pageant nor OpenSSH's agent is running]\r\n");
	Ssh *c = (Ssh *)calloc(1, sizeof(Ssh));
	c->s = s;
	DWORD id;
	c->thread = CreateThread(NULL, 0, ssh_poll, c, 0, &id);
	if (!c->thread) {
		dmc_ssh_free(s);
		free(c);
		app_message("Could not start SSH");
		return false;
	}
	ssh = c;
	shell = 0;
	tried_keys = 0;
	tried_file = 0;
	text_free(key_text);
	key_text = NULL;
	asks = 0;
	exit_status = 0;
	return true;
}

/* the profile's .ssh folder (made if it isn't there) into out */
static void ssh_dir(char *out, int n)
{
	const char *home = getenv("USERPROFILE");
	_snprintf(out, n, "%s\\.ssh", home ? home : ".");
	out[n - 1] = '\0';
	CreateDirectory(out, NULL);
}

/* a key file chosen into the dialog's field (ppk: PuTTY's alone) */
static bool choose_key(HWND d, bool ppk, char *file, int n)
{
	char dir[MAX_PATH];
	ssh_dir(dir, sizeof(dir));
	OPENFILENAME o;
	memset(&o, 0, sizeof(o));
	o.lStructSize = sizeof(o);	/* (the SSH build is for Windows 7 on) */
	o.hwndOwner = d;
	o.lpstrFilter = ppk ? "PuTTY keys (*.ppk)\0*.ppk\0" :
		"All keys\0*.*\0PuTTY keys (*.ppk)\0*.ppk\0";
	o.lpstrFile = file;
	o.nMaxFile = (DWORD)n;
	o.lpstrInitialDir = dir;
	o.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
	file[0] = '\0';
	return GetOpenFileName(&o) != 0;
}

/* a text written to path: true if it all was */
static bool text_write(const char *path, const char *t)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return false;
	size_t n = strlen(t);
	bool ok = fwrite(t, 1, n, f) == n;
	return fclose(f) == 0 && ok;
}

/* a .ppk imported: written to .ssh in OpenSSH's form (with the same
   passphrase), its name into out; false if not */
static bool import_ppk(HWND d, const char *ppk, char *out, int n)
{
	char *text = text_read(ppk);
	if (!text) {
		MessageBox(d, "That file can't be read.", "Import PuTTY Key", MB_ICONWARNING);
		return false;
	}
	char pass[256] = "", *key = NULL, *pub = NULL;
	bool enc = dmc_ssh_key_encrypted(text) != 0;
	const char *why = "";
	for (;;) {
		if (enc && !ask_passphrase(d, ppk, why, "OK", pass, sizeof(pass)))
			break;
		if (dmc_ssh_key_export(text, pass, pass, &key) == 0 &&
		    dmc_ssh_key_public(key, pass, &pub) == 0)
			break;
		if (key) {
			SecureZeroMemory(key, strlen(key));
			dmc_ssh_free_string(key);
			key = NULL;
		}
		if (!enc) {
			MessageBox(d, "That file is not a PuTTY key that can be read.",
				   "Import PuTTY Key", MB_ICONWARNING);
			break;
		}
		why = "That passphrase does not open the key.";
	}
	SecureZeroMemory(pass, sizeof(pass));
	bool ok = false;
	if (key && pub) {
		/* (.ssh\<its name>, a name not yet taken) */
		char dir[MAX_PATH], base[MAX_PATH], path[MAX_PATH], pubpath[MAX_PATH + 8];
		ssh_dir(dir, sizeof(dir));
		const char *slash = strrchr(ppk, '\\');
		lstrcpyn(base, slash ? slash + 1 : ppk, sizeof(base));
		char *dot = strrchr(base, '.');
		if (dot && !lstrcmpi(dot, ".ppk"))
			*dot = '\0';
		if (!base[0])
			lstrcpy(base, "putty-key");
		for (int i = 1; i < 100; i++) {
			if (i == 1)
				_snprintf(path, sizeof(path), "%s\\%s", dir, base);
			else
				_snprintf(path, sizeof(path), "%s\\%s-%d", dir, base, i);
			path[sizeof(path) - 1] = '\0';
			_snprintf(pubpath, sizeof(pubpath), "%s.pub", path);
			pubpath[sizeof(pubpath) - 1] = '\0';
			if (GetFileAttributes(path) == INVALID_FILE_ATTRIBUTES &&
			    GetFileAttributes(pubpath) == INVALID_FILE_ATTRIBUTES)
				break;
		}
		/* (the .pub line with the .ppk's comment) */
		char line[4096], comment[256] = "";
		const char *cm = strstr(text, "\nComment: ");
		if (cm) {
			int cl = (int)strcspn(cm + 10, "\r\n");
			_snprintf(comment, sizeof(comment), " %.*s", cl, cm + 10);
			comment[sizeof(comment) - 1] = '\0';
		}
		_snprintf(line, sizeof(line), "%s%s\n", pub, comment);
		line[sizeof(line) - 1] = '\0';
		ok = text_write(path, key) && text_write(pubpath, line);
		if (ok) {
			lstrcpyn(out, path, n);
			char m[MAX_PATH + 96];
			_snprintf(m, sizeof(m), "Imported as %s, and set as the key file.", path);
			m[sizeof(m) - 1] = '\0';
			MessageBox(d, m, "Import PuTTY Key", MB_ICONINFORMATION);
		} else {
			MessageBox(d, "The key could not be saved in .ssh.", "Import PuTTY Key",
				   MB_ICONWARNING);
		}
	}
	if (key) {
		SecureZeroMemory(key, strlen(key));
		dmc_ssh_free_string(key);
	}
	dmc_ssh_free_string(pub);
	text_free(text);
	return ok;
}

/* whether the agent typed answers, under it */
static void agent_state(HWND d)
{
	char a[MAX_PATH];
	GetDlgItemText(d, IDC_K_AGENT, a, sizeof(a));
	SetDlgItemText(d, IDC_K_STATE, dmc_ssh_agent_reachable(a) ?
		       (a[0] ? "An agent answers there." :
			FindWindow("Pageant", "Pageant") ? "Pageant is running." :
			"OpenSSH's agent is running.") :
		       a[0] ? "No agent answers there." :
		       "Neither Pageant nor OpenSSH's agent is running.");
}

/* the ways in the list's order, as dmodem.ini has them */
static const char *const hows[] = { "auto", "agent", "key" };

static INT_PTR CALLBACK login_proc(HWND d, UINT msg, WPARAM wp, LPARAM lp)
{
	Login *l = (Login *)GetWindowLongPtr(d, DWLP_USER);
	switch (msg) {
	case WM_INITDIALOG: {
		l = (Login *)lp;
		SetWindowLongPtr(d, DWLP_USER, (LONG_PTR)l);
		HWND how = GetDlgItem(d, IDC_K_HOW);
		SendMessage(how, CB_ADDSTRING, 0, (LPARAM)"My keys and the agent, as ssh does");
		SendMessage(how, CB_ADDSTRING, 0, (LPARAM)"The agent's keys only");
		SendMessage(how, CB_ADDSTRING, 0, (LPARAM)"A key file");
		int sel = 0;
		for (int i = 0; i < 3; i++)
			if (!lstrcmpi(l->how, hows[i]))
				sel = i;
		SendMessage(how, CB_SETCURSEL, (WPARAM)sel, 0);
		SetDlgItemText(d, IDC_K_KEY, l->key);
		SetDlgItemText(d, IDC_K_AGENT, l->agent);
		agent_state(d);
		return TRUE;
	}
	case WM_COMMAND:
		switch (LOWORD(wp)) {
		case IDC_K_BROWSE: {
			char f[MAX_PATH];
			if (choose_key(d, false, f, sizeof(f))) {
				SetDlgItemText(d, IDC_K_KEY, f);
				SendDlgItemMessage(d, IDC_K_HOW, CB_SETCURSEL, 2, 0);
			}
			return TRUE;
		}
		case IDC_K_IMPORT: {
			char f[MAX_PATH], out[MAX_PATH];
			if (choose_key(d, true, f, sizeof(f)) && import_ppk(d, f, out, sizeof(out))) {
				SetDlgItemText(d, IDC_K_KEY, out);
				SendDlgItemMessage(d, IDC_K_HOW, CB_SETCURSEL, 2, 0);
			}
			return TRUE;
		}
		case IDC_K_AGENT:
			if (HIWORD(wp) == EN_CHANGE)
				agent_state(d);
			return TRUE;
		case IDOK: {
			int i = (int)SendDlgItemMessage(d, IDC_K_HOW, CB_GETCURSEL, 0, 0);
			lstrcpyn(l->how, hows[i >= 0 && i < 3 ? i : 0], sizeof(l->how));
			GetDlgItemText(d, IDC_K_KEY, l->key, sizeof(l->key));
			GetDlgItemText(d, IDC_K_AGENT, l->agent, sizeof(l->agent));
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

/* the SSH Login dialog: how SSH logs in, kept */
static void login_clicked()
{
	Login l;
	login_read(&l);
	if (DialogBoxParam(app.inst, MAKEINTRESOURCE(IDD_LOGIN), app.main,
			   login_proc, (LPARAM)&l) == IDOK)
		login_write(&l);
}

#else

static bool start_ssh(const char *host, int port, const char *user)
{
	if (!conpty_available()) {
		app_message("SSH needs Windows 10 (1809) or later, with "
			    "OpenSSH's client");
		return false;
	}
	char ssh[MAX_PATH];
	if (!find_ssh(ssh, sizeof(ssh))) {
		app_message("ssh.exe was not found: add Windows' OpenSSH "
			    "Client (Settings, Optional features)");
		return false;
	}
	HANDLE in_r, in_w, out_r, out_w;
	if (!CreatePipe(&in_r, &in_w, NULL, 0))
		return false;
	if (!CreatePipe(&out_r, &out_w, NULL, 0)) {
		CloseHandle(in_r);
		CloseHandle(in_w);
		return false;
	}
	COORD sz;
	sz.X = (SHORT)cols;
	sz.Y = (SHORT)rows;
	if (create_pc(sz, in_r, out_w, 0, &hpc) < 0) {
		hpc = NULL;
		CloseHandle(in_r);
		CloseHandle(in_w);
		CloseHandle(out_r);
		CloseHandle(out_w);
		app_message("Could not make a console for ssh");
		return false;
	}
	/* (the console has its own copies) */
	CloseHandle(in_r);
	CloseHandle(out_w);
	con_in = in_w;
	con_out = out_r;

	size_t size = 0;
	init_attrs(NULL, 1, 0, &size);
	void *attrs = malloc(size);
	StartupInfoEx si;
	memset(&si, 0, sizeof(si));
	si.si.cb = sizeof(si);
	si.attrs = attrs;
	bool ok = attrs && init_attrs(attrs, 1, 0, &size) &&
		update_attr(attrs, 0, ATTR_PSEUDOCONSOLE, hpc, sizeof(hpc),
			    NULL, NULL);
	char cmd[1024];
	if (*user)
		_snprintf(cmd, sizeof(cmd), "\"%s\" -p %d -l \"%s\" -- \"%s\"",
			  ssh, port, user, host);
	else
		_snprintf(cmd, sizeof(cmd), "\"%s\" -p %d -- \"%s\"", ssh, port,
			  host);
	cmd[sizeof(cmd) - 1] = '\0';
	PROCESS_INFORMATION pi;
	memset(&pi, 0, sizeof(pi));
	SetEnvironmentVariable("TERM",
			       dmc_vt_profile_term(termview_profile(view)));
	if (ok)
		ok = CreateProcess(NULL, cmd, NULL, NULL, FALSE,
				   EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
				   (STARTUPINFOA *)&si, &pi) != 0;
	if (attrs) {
		delete_attrs(attrs);
		free(attrs);
	}
	if (!ok) {
		running = DMC_NET_SSH;  /* (for ended() to tidy up) */
		ended("Could not start ssh.exe");
		return false;
	}
	CloseHandle(pi.hThread);
	proc = pi.hProcess;
	return true;
}

void remote_ssh(LPARAM l)
{
	(void)l;
}
#endif

static bool start_socket(int proto, const char *host, int port,
			 const char *user)
{
	sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((u_short)port);
	sa.sin_addr.s_addr = inet_addr(host);
	if (sa.sin_addr.s_addr == INADDR_NONE) {
		hostent *h = gethostbyname(host);
		if (!h) {
			app_message("That server's name was not found");
			return false;
		}
		memcpy(&sa.sin_addr, h->h_addr, sizeof(sa.sin_addr));
	}
	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock == INVALID_SOCKET)
		return false;
	/* (Telnet's TERMINAL-TYPE, rlogin's TERM) */
	const char *type = termview_profile(view);
	net = dmc_net_new(proto, user, proto == DMC_NET_TELNET ?
			  dmc_vt_profile_telnet(type) :
			  dmc_vt_profile_term(type), cols, rows);
	sock_open = false;
	WSAAsyncSelect(sock, app.main, WM_APP_REMOTE,
		       FD_CONNECT | FD_READ | FD_WRITE | FD_CLOSE | FD_OOB);
	connect(sock, (sockaddr *)&sa, sizeof(sa));
	return true;
}

static int chosen_proto()
{
	int i = (int)SendMessage(proto_box, CB_GETCURSEL, 0, 0);
	return i >= 0 && i < NPROTOS ? protos[i] : DMC_NET_SSH;
}

static void ini(char *out, int n)
{
	char dir[MAX_PATH];
	app_data_dir(dir, sizeof(dir));
	_snprintf(out, n, "%s\\dmodem.ini", dir);
	out[n - 1] = '\0';
}

static void connect_clicked()
{
	if (running) {
		ended(NULL);
		return;
	}
	char host[256], user[128], pt[16];
	GetWindowText(host_ed, host, sizeof(host));
	GetWindowText(user_ed, user, sizeof(user));
	GetWindowText(port_ed, pt, sizeof(pt));
	int proto = chosen_proto();
	int port = pt[0] ? atoi(pt) : dmc_net_default_port(proto);
	if (!host[0]) {
		app_message("Enter the server to connect to");
		return;
	}
	if (strpbrk(host, " \"") || strchr(user, '"')) {
		app_message("The server's name has no spaces or quotes");
		return;
	}
	if (port <= 0 || port > 65535) {
		app_message("The port is a number from 1 to 65535");
		return;
	}
	if (proto == DMC_NET_RLOGIN && !user[0]) {
		app_message("rlogin needs a user name");
		return;
	}
	/* kept for next time */
	char path[MAX_PATH];
	ini(path, sizeof(path));
	WritePrivateProfileString("remote", "protocol", dmc_net_name(proto), path);
	WritePrivateProfileString("remote", "host", host, path);
	WritePrivateProfileString("remote", "port", pt, path);
	WritePrivateProfileString("remote", "user", user, path);
	WritePrivateProfileString("remote", "terminal", termview_profile(view),
				  path);

	termview_reset(view);
	termview_size(view, &cols, &rows);
	char what[400];
	_snprintf(what, sizeof(what), "Connecting to %s port %d by %s", host,
		  port, dmc_net_name(proto));
	what[sizeof(what) - 1] = '\0';
	set_status(what);
	char line[420];
	_snprintf(line, sizeof(line), "[%s]\r\n", what);
	line[sizeof(line) - 1] = '\0';
	show_line(line);
	bool ok = proto == DMC_NET_SSH ? start_ssh(host, port, user) :
		start_socket(proto, host, port, user);
	if (!ok) {
		set_status("Not connected");
		return;
	}
#ifndef DMODEM_HAVE_SSH
	if (proto == DMC_NET_SSH)
		set_status("Connected by SSH");
#endif
	running = proto;
	SetTimer(app.main, TIMER_REMOTE, 30, NULL);
	controls();
	SetFocus(view);
}

static void proto_changed()
{
	char p[16];
	_snprintf(p, sizeof(p), "%d", dmc_net_default_port(chosen_proto()));
	p[sizeof(p) - 1] = '\0';
	/* (the default shown when the port is left empty) */
	char cur[16];
	GetWindowText(port_ed, cur, sizeof(cur));
	if (!cur[0] || !strcmp(cur, "22") || !strcmp(cur, "23") ||
	    !strcmp(cur, "513"))
		SetWindowText(port_ed, p);
	controls();
}

static void remote_command(HWND p, int id, int code)
{
	(void)p;
	switch (id) {
	case IDC_R_GO:
		connect_clicked();
		break;
#ifdef DMODEM_HAVE_SSH
	case IDC_R_LOGIN:
		login_clicked();
		break;
#endif
	case IDC_R_PROTO:
		if (code == CBN_SELCHANGE)
			proto_changed();
		break;
	case IDC_R_TYPE:
		/* (the far end was told the type it had when it connected) */
		if (code == CBN_SELCHANGE)
			termview_set_profile(view, term_types_chosen(type_box));
		break;
	}
}

HWND remote_page_new(HWND parent)
{
	page = page_new(parent, remote_command);
	proto_box = make_child("COMBOBOX", "", WS_TABSTOP | CBS_DROPDOWNLIST |
			       WS_VSCROLL, page, IDC_R_PROTO);
	for (int i = 0; i < NPROTOS; i++)
		SendMessage(proto_box, CB_ADDSTRING, 0,
			    (LPARAM)dmc_net_name(protos[i]));
	make_child("STATIC", "&Server:", SS_RIGHT, page, IDC_R_HOSTLABEL);
	host_ed = make_child("EDIT", "", WS_TABSTOP | ES_AUTOHSCROLL, page,
			     IDC_R_HOST, WS_EX_CLIENTEDGE);
	make_child("STATIC", "&Port:", SS_RIGHT, page, IDC_R_PORTLABEL);
	port_ed = make_child("EDIT", "", WS_TABSTOP | ES_NUMBER, page,
			     IDC_R_PORT, WS_EX_CLIENTEDGE);
	make_child("STATIC", "&User:", SS_RIGHT, page, IDC_R_USERLABEL);
	user_ed = make_child("EDIT", "", WS_TABSTOP | ES_AUTOHSCROLL, page,
			     IDC_R_USER, WS_EX_CLIENTEDGE);
	go_btn = make_child("BUTTON", "&Connect", WS_TABSTOP | BS_DEFPUSHBUTTON,
			    page, IDC_R_GO);
#ifdef DMODEM_HAVE_SSH
	/* (how SSH logs in: keys, the agent, a PuTTY key) */
	login_btn = make_child("BUTTON", "&Login...", WS_TABSTOP, page, IDC_R_LOGIN);
#endif
	view = termview_new(page, IDC_R_TERM, typed, NULL);
	status_txt = make_child("STATIC", "Not connected", SS_LEFTNOWORDWRAP,
				page, IDC_R_STATUS);
	make_child("STATIC", "&Terminal:", SS_RIGHT, page, IDC_R_TYPELABEL);
	type_box = make_child("COMBOBOX", "", WS_TABSTOP | CBS_DROPDOWNLIST |
			      WS_VSCROLL, page, IDC_R_TYPE);

	/* what was used last */
	char path[MAX_PATH], v[256];
	ini(path, sizeof(path));
	GetPrivateProfileString("remote", "protocol", "SSH", v, sizeof(v), path);
	int sel = 0;
	for (int i = 0; i < NPROTOS; i++)
		if (!lstrcmpi(v, dmc_net_name(protos[i])))
			sel = i;
	SendMessage(proto_box, CB_SETCURSEL, (WPARAM)sel, 0);
	GetPrivateProfileString("remote", "host", "", v, sizeof(v), path);
	SetWindowText(host_ed, v);
	GetPrivateProfileString("remote", "port", "", v, sizeof(v), path);
	SetWindowText(port_ed, v);
	GetPrivateProfileString("remote", "user", "", v, sizeof(v), path);
	SetWindowText(user_ed, v);
	GetPrivateProfileString("remote", "terminal", "xterm", v, sizeof(v),
				path);
	term_types_fill(type_box, v);
	termview_set_profile(view, term_types_chosen(type_box));
	proto_changed();
	controls();
	return page;
}

void remote_page_layout(HWND p, int w, int h)
{
	int m = 8, bh = 24, x = m;
	MoveWindow(proto_box, x, m, 80, 200, TRUE);
	x += 88;
	MoveWindow(GetDlgItem(p, IDC_R_HOSTLABEL), x, m + 4, 44, bh, TRUE);
	x += 48;
	int hw = w - x - 44 - 52 - 40 - 96 - 90 - 4 * m - (login_btn ? 64 + m : 0);
	if (hw < 80)
		hw = 80;
	MoveWindow(host_ed, x, m, hw, bh, TRUE);
	x += hw + m;
	MoveWindow(GetDlgItem(p, IDC_R_PORTLABEL), x, m + 4, 32, bh, TRUE);
	x += 36;
	MoveWindow(port_ed, x, m, 52, bh, TRUE);
	x += 52 + m;
	MoveWindow(GetDlgItem(p, IDC_R_USERLABEL), x, m + 4, 36, bh, TRUE);
	x += 40;
	MoveWindow(user_ed, x, m, 96, bh, TRUE);
	MoveWindow(go_btn, w - m - 90, m, 90, bh, TRUE);
	if (login_btn)
		MoveWindow(login_btn, w - 2 * m - 90 - 64, m, 64, bh, TRUE);
	MoveWindow(view, m, 2 * m + bh, w - 2 * m, h - 4 * m - 2 * bh, TRUE);
	int tw = 200;
	MoveWindow(status_txt, m, h - m - bh + 4, w - 3 * m - tw - 64, bh - 4,
		   TRUE);
	MoveWindow(GetDlgItem(p, IDC_R_TYPELABEL), w - m - tw - 64, h - m - bh + 4,
		   60, bh - 4, TRUE);
	MoveWindow(type_box, w - m - tw, h - m - bh, tw, 300, TRUE);
}

bool remote_focus()
{
	SetFocus(running ? view : host_ed);
	return true;
}

void remote_stop()
{
	if (running)
		ended(NULL);
}
