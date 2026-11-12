/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * remote.cpp - the Remote page: a login to a server by SSH, Telnet or
 *              rlogin, in a terminal of its own (termview.cpp)
 *
 * Telnet and rlogin: a Winsock 1.1 socket (WSAAsyncSelect, as the
 * modem's link), the protocol libdmodemctl's dmc_net; every Windows from
 * 95 on has them.  SSH: Windows' own OpenSSH client, ssh.exe, in a
 * pseudo console (ConPTY), so it needs Windows 10 1809 or later with
 * the OpenSSH client feature; the ConPTY calls are looked up at run
 * time, so that the app still starts on Windows 95.  Its output is read
 * from a timer (PeekNamedPipe), with no threads.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "resource.h"

static HWND page, view, proto_box, host_ed, port_ed, user_ed, go_btn,
	status_txt;

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
	} else if (con_in != INVALID_HANDLE_VALUE) {
		DWORD w;
		WriteFile(con_in, data, (DWORD)n, &w, NULL);
	}
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
		} else if (hpc) {
			COORD sz;
			sz.X = (SHORT)c;
			sz.Y = (SHORT)r;
			resize_pc(hpc, sz);
		}
	}
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
}

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
	SetEnvironmentVariable("TERM", "xterm");
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
	net = dmc_net_new(proto, user, "ansi", cols, rows);
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
	if (proto == DMC_NET_SSH)
		set_status("Connected by SSH");
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
}

static void remote_command(HWND p, int id, int code)
{
	(void)p;
	switch (id) {
	case IDC_R_GO:
		connect_clicked();
		break;
	case IDC_R_PROTO:
		if (code == CBN_SELCHANGE)
			proto_changed();
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
	view = termview_new(page, IDC_R_TERM, typed, NULL);
	status_txt = make_child("STATIC", "Not connected", SS_LEFTNOWORDWRAP,
				page, IDC_R_STATUS);

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
	int hw = w - x - 44 - 52 - 40 - 96 - 90 - 4 * m;
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
	MoveWindow(view, m, 2 * m + bh, w - 2 * m, h - 4 * m - 2 * bh + 6, TRUE);
	MoveWindow(status_txt, m, h - m - bh + 6, w - 2 * m, bh - 6, TRUE);
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
