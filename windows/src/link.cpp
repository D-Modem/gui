/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * link.cpp - the modem's connection: dmodemd started here (its DTE a
 *            TCP socket on 127.0.0.1), a TCP address, or a COM port;
 *            and the bytes between it, the session and the terminal
 *
 * Only what Windows 95 has: Winsock 1.1 (WSAAsyncSelect), anonymous
 * pipes read with PeekNamedPipe, and a COM port read on the timer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"

/* the commands that wait for their responses */
struct Waiting {
	int tag;
	ResponseFn fn;
	void *data;
	Waiting *next;
};

static Waiting *waiting;
static SOCKET sock = INVALID_SOCKET;
static int sock_ready;                  /* connected */
static HANDLE com = INVALID_HANDLE_VALUE;
static HANDLE proc, proc_out;           /* dmodemd, its stdout and stderr */
static char proc_line[1024];
static int proc_n;
static char port_out[8192];             /* what the DTE has yet to take */
static int port_out_n;
static DWORD start_tick;
static int connect_tries;

static int64_t now_ms()
{
	return (int64_t)GetTickCount();
}

bool link_open()
{
	return (sock != INVALID_SOCKET && sock_ready) ||
		com != INVALID_HANDLE_VALUE;
}

bool link_running()
{
	return proc != NULL || sock != INVALID_SOCKET ||
		com != INVALID_HANDLE_VALUE;
}

static void opened(const char *what)
{
	_snprintf(app.device, sizeof(app.device), "%s", what);
	app.device[sizeof(app.device) - 1] = '\0';
	dmc_open(app.s, now_ms());
	if (app.cfg.init[0])
		link_command(app.cfg.init, 0, NULL, NULL);
	app_opened();
	link_pump();
}

static void close_port()
{
	if (sock != INVALID_SOCKET) {
		closesocket(sock);
		sock = INVALID_SOCKET;
	}
	sock_ready = 0;
	if (com != INVALID_HANDLE_VALUE) {
		CloseHandle(com);
		com = INVALID_HANDLE_VALUE;
	}
	port_out_n = 0;
	while (waiting) {
		Waiting *w = waiting;
		waiting = w->next;
		free(w);
	}
	if (dmc_state(app.s) != DMC_CLOSED) {
		dmc_close(app.s);
		dmc_event ev;
		while (dmc_next_event(app.s, &ev))
			;
		app_closed();
	}
}

/* host:port, its address */
static bool address(const char *spec, sockaddr_in *sa)
{
	char host[256];
	strncpy(host, spec, sizeof(host) - 1);
	host[sizeof(host) - 1] = '\0';
	char *colon = strrchr(host, ':');
	int port = 2323;
	if (colon) {
		*colon = '\0';
		port = atoi(colon + 1);
	}
	memset(sa, 0, sizeof(*sa));
	sa->sin_family = AF_INET;
	sa->sin_port = htons((u_short)port);
	sa->sin_addr.s_addr = inet_addr(host);
	if (sa->sin_addr.s_addr == INADDR_NONE) {
		hostent *h = gethostbyname(host);
		if (!h)
			return false;
		memcpy(&sa->sin_addr, h->h_addr, sizeof(sa->sin_addr));
	}
	return true;
}

static bool open_tcp(const char *spec)
{
	sockaddr_in sa;
	if (!address(spec, &sa)) {
		char m[300];
		_snprintf(m, sizeof(m), "Can't find %s", spec);
		m[sizeof(m) - 1] = '\0';
		app_message(m);
		app_log(m);
		return false;
	}
	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock == INVALID_SOCKET)
		return false;
	WSAAsyncSelect(sock, app.main, WM_APP_SOCKET,
		       FD_CONNECT | FD_READ | FD_WRITE | FD_CLOSE);
	connect(sock, (sockaddr *)&sa, sizeof(sa));
	if (spec != app.device)
		_snprintf(app.device, sizeof(app.device), "%s", spec);
	app.device[sizeof(app.device) - 1] = '\0';
	return true;
}

static bool open_com(const char *name)
{
	char path[64];
	_snprintf(path, sizeof(path), "\\\\.\\%s", name);
	path[sizeof(path) - 1] = '\0';
	com = CreateFile(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
			 OPEN_EXISTING, 0, NULL);
	if (com == INVALID_HANDLE_VALUE) {
		char m[128];
		_snprintf(m, sizeof(m), "Can't open %s (error %lu)", name,
			  (unsigned long)GetLastError());
		m[sizeof(m) - 1] = '\0';
		app_message(m);
		app_log(m);
		return false;
	}
	DCB dcb;
	memset(&dcb, 0, sizeof(dcb));
	dcb.DCBlength = sizeof(dcb);
	GetCommState(com, &dcb);
	dcb.BaudRate = CBR_115200;
	dcb.ByteSize = 8;
	dcb.Parity = NOPARITY;
	dcb.StopBits = ONESTOPBIT;
	dcb.fBinary = TRUE;
	dcb.fDtrControl = DTR_CONTROL_ENABLE;
	dcb.fRtsControl = RTS_CONTROL_HANDSHAKE;
	dcb.fOutxCtsFlow = TRUE;
	SetCommState(com, &dcb);
	/* reads return at once with what there is */
	COMMTIMEOUTS t;
	memset(&t, 0, sizeof(t));
	t.ReadIntervalTimeout = MAXDWORD;
	t.WriteTotalTimeoutConstant = 1000;
	SetCommTimeouts(com, &t);
	opened(name);
	return true;
}

/* dmodemd.exe: the configured one, or next to this app */
static void dmodemd_path(char *out, int n)
{
	if (app.cfg.dmodemd[0]) {
		_snprintf(out, n, "%s", app.cfg.dmodemd);
		out[n - 1] = '\0';
		return;
	}
	GetModuleFileName(NULL, out, n);
	char *slash = strrchr(out, '\\');
	if (slash)
		strcpy(slash + 1, "dmodemd.exe");
}

/* an environment block: ours, and these */
static char *environment(const char *const *add)
{
	char *env = GetEnvironmentStrings();
	int n = 0;
	while (env[n])
		n += (int)strlen(env + n) + 1;
	int extra = 0;
	for (int i = 0; add[i]; i++)
		extra += (int)strlen(add[i]) + 1;
	char *block = (char *)malloc(n + extra + 2);
	int o = 0;
	for (int i = 0; add[i]; i++) {
		strcpy(block + o, add[i]);
		o += (int)strlen(add[i]) + 1;
	}
	/* (ours, but for those set here) */
	for (int p = 0; env[p]; p += (int)strlen(env + p) + 1) {
		bool dup = false;
		for (int i = 0; add[i]; i++) {
			const char *eq = strchr(add[i], '=');
			if (eq && !_strnicmp(env + p, add[i], eq - add[i] + 1))
				dup = true;
		}
		if (!dup) {
			strcpy(block + o, env + p);
			o += (int)strlen(env + p) + 1;
		}
	}
	block[o] = '\0';
	FreeEnvironmentStrings(env);
	return block;
}

static bool start_dmodemd()
{
	Config &c = app.cfg;
	if (!c.usb[0] && (!c.sip_user[0] || !c.sip_server[0])) {
		app_message("Set up the SIP account first (Modem, Line Settings)");
		return false;
	}
	char exe[MAX_PATH];
	dmodemd_path(exe, sizeof(exe));
	if (GetFileAttributes(exe) == (DWORD)-1) {
		char m[MAX_PATH + 64];
		_snprintf(m, sizeof(m), "dmodemd.exe was not found (%s)", exe);
		m[sizeof(m) - 1] = '\0';
		app_message(m);
		app_log(m);
		return false;
	}
	/* a port of our own on 127.0.0.1 */
	int port = 23000 + (int)(GetCurrentProcessId() % 1000);
	char cmd[1024];
	int o = _snprintf(cmd, sizeof(cmd), "\"%s\" --tcp 127.0.0.1:%d", exe,
			  port);
	if (c.country[0])
		o += _snprintf(cmd + o, sizeof(cmd) - o, " -c %s", c.country);
	if (c.usb[0])
		o += _snprintf(cmd + o, sizeof(cmd) - o, " --usb=%s", c.usb);
	else if (c.listen)
		o += _snprintf(cmd + o, sizeof(cmd) - o, " -L");
	cmd[sizeof(cmd) - 1] = '\0';

	char login[600];
	_snprintf(login, sizeof(login), "SIP_LOGIN=%s:%s@%s", c.sip_user,
		  c.sip_password, c.sip_server);
	login[sizeof(login) - 1] = '\0';
	const char *add[6];
	int k = 0;
	if (!c.usb[0])
		add[k++] = login;
	if (!c.t38)
		add[k++] = "SIP_T38=no";
	if (!c.v152)
		add[k++] = "SIP_V152=no";
	if (c.v150)
		add[k++] = "SIP_V150=yes";
	add[k] = NULL;
	char *env = environment(add);

	SECURITY_ATTRIBUTES sa;
	memset(&sa, 0, sizeof(sa));
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;
	HANDLE rd, wr;
	if (!CreatePipe(&rd, &wr, &sa, 0)) {
		free(env);
		return false;
	}
	/* our end not inherited (DuplicateHandle, as Windows 95 has no
	   SetHandleInformation) */
	HANDLE rd2;
	if (DuplicateHandle(GetCurrentProcess(), rd, GetCurrentProcess(), &rd2,
			    0, FALSE, DUPLICATE_SAME_ACCESS)) {
		CloseHandle(rd);
		rd = rd2;
	}
	STARTUPINFO si;
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	si.hStdOutput = wr;
	si.hStdError = wr;
	PROCESS_INFORMATION pi;
	BOOL ok = CreateProcess(NULL, cmd, NULL, NULL, TRUE,
				CREATE_NO_WINDOW, env, NULL, &si, &pi);
	free(env);
	CloseHandle(wr);
	char m[1100];
	if (!ok) {
		CloseHandle(rd);
		_snprintf(m, sizeof(m), "Can't start dmodemd (error %lu)",
			  (unsigned long)GetLastError());
		m[sizeof(m) - 1] = '\0';
		app_message(m);
		app_log(m);
		return false;
	}
	_snprintf(m, sizeof(m), "> %s", cmd);
	m[sizeof(m) - 1] = '\0';
	app_log(m);
	CloseHandle(pi.hThread);
	proc = pi.hProcess;
	proc_out = rd;
	proc_n = 0;
	start_tick = GetTickCount();
	connect_tries = 0;
	return true;
}

void link_start()
{
	if (link_running())
		return;
	bool ok = false;
	switch (app.cfg.line) {
	case LINE_TCP:
		ok = open_tcp(app.cfg.tcp);
		break;
	case LINE_COM:
		ok = open_com(app.cfg.com);
		break;
	default:
		ok = start_dmodemd();
		break;
	}
	(void)ok;
	app_state();
}

void link_stop()
{
	if (link_open()) {
		dmc_hangup(app.s);
		link_pump();
	}
	close_port();
	if (proc) {
		/* dmodemd has no window to close: it is ended */
		TerminateProcess(proc, 0);
		CloseHandle(proc);
		proc = NULL;
	}
	if (proc_out) {
		CloseHandle(proc_out);
		proc_out = NULL;
	}
	app_state();
}

/* dmodemd's output, a line at a time */
static void proc_read()
{
	DWORD avail = 0;
	while (proc_out && PeekNamedPipe(proc_out, NULL, 0, NULL, &avail, NULL) &&
	       avail) {
		char buf[512];
		DWORD n = 0;
		if (!ReadFile(proc_out, buf, avail < sizeof(buf) ? avail :
			      sizeof(buf), &n, NULL) || !n)
			break;
		for (DWORD i = 0; i < n; i++) {
			char ch = buf[i];
			if (ch == '\r')
				continue;
			if (ch != '\n' && proc_n < (int)sizeof(proc_line) - 1) {
				proc_line[proc_n++] = ch;
				continue;
			}
			if (ch != '\n')
				continue;
			proc_line[proc_n] = '\0';
			proc_n = 0;
			app_log(proc_line);
			char dev[256];
			if (dmc_daemon_ready(proc_line, dev, sizeof(dev)) &&
			    !link_open() && sock == INVALID_SOCKET) {
				/* "TCP 127.0.0.1:23001" */
				const char *a = strncmp(dev, "TCP ", 4) ? dev :
					dev + 4;
				open_tcp(a);
			} else if (dmc_daemon_error(proc_line) && !link_open()) {
				app_message(proc_line + 7);
			}
		}
	}
	if (proc && WaitForSingleObject(proc, 0) == WAIT_OBJECT_0) {
		DWORD code = 0;
		GetExitCodeProcess(proc, &code);
		char m[64];
		_snprintf(m, sizeof(m), "dmodemd stopped (exit status %ld)",
			  (long)code);
		m[sizeof(m) - 1] = '\0';
		app_log(m);
		app_message(m);
		CloseHandle(proc);
		proc = NULL;
		close_port();
		if (proc_out) {
			CloseHandle(proc_out);
			proc_out = NULL;
		}
		app_state();
	}
}

static void port_write()
{
	int sent = 0;
	while (sent < port_out_n) {
		int n = 0;
		if (sock != INVALID_SOCKET && sock_ready) {
			n = send(sock, port_out + sent, port_out_n - sent, 0);
			if (n == SOCKET_ERROR)
				n = 0;
		} else if (com != INVALID_HANDLE_VALUE) {
			DWORD w = 0;
			if (!WriteFile(com, port_out + sent, port_out_n - sent,
				       &w, NULL))
				w = 0;
			n = (int)w;
		}
		if (n <= 0)
			break;
		sent += n;
	}
	memmove(port_out, port_out + sent, port_out_n - sent);
	port_out_n -= sent;
}

static void port_read()
{
	char buf[4096];
	if (com != INVALID_HANDLE_VALUE) {
		DWORD n = 0;
		while (ReadFile(com, buf, sizeof(buf), &n, NULL) && n > 0)
			dmc_feed(app.s, buf, (int)n);
	}
}

void link_socket(WPARAM w, LPARAM l)
{
	if ((SOCKET)w != sock)
		return;
	int ev = WSAGETSELECTEVENT(l), err = WSAGETSELECTERROR(l);
	if (ev == FD_CONNECT) {
		if (err) {
			closesocket(sock);
			sock = INVALID_SOCKET;
			/* dmodemd may not listen yet: try again for a while */
			if (proc && connect_tries++ < 20) {
				Sleep(100);
				open_tcp(app.device);
				return;
			}
			char m[300];
			_snprintf(m, sizeof(m), "Can't connect to %s (error %d)",
				  app.device, err);
			m[sizeof(m) - 1] = '\0';
			app_message(m);
			app_log(m);
			app_state();
			return;
		}
		sock_ready = 1;
		char what[300];
		_snprintf(what, sizeof(what), "%s (TCP)", app.device);
		what[sizeof(what) - 1] = '\0';
		opened(what);
	} else if (ev == FD_READ) {
		char buf[4096];
		int n = recv(sock, buf, sizeof(buf), 0);
		if (n > 0)
			dmc_feed(app.s, buf, n);
		link_pump();
	} else if (ev == FD_WRITE) {
		port_write();
	} else if (ev == FD_CLOSE) {
		app_log("The modem closed the connection.");
		close_port();
		app_state();
	}
}

void link_pump()
{
	if (proc_out)
		proc_read();
	if (!link_open())
		return;
	port_read();
	dmc_poll(app.s, now_ms());
	char buf[4096];
	int n;
	while ((n = dmc_output(app.s, buf, sizeof(buf))) > 0) {
		if (port_out_n + n > (int)sizeof(port_out))
			n = (int)sizeof(port_out) - port_out_n;
		memcpy(port_out + port_out_n, buf, n);
		port_out_n += n;
	}
	port_write();
	while ((n = dmc_terminal(app.s, buf, sizeof(buf))) > 0)
		term_feed(buf, n);
	dmc_event ev;
	while (dmc_next_event(app.s, &ev)) {
		if (ev.type != DMC_EV_RESPONSE) {
			app_event(&ev);
			continue;
		}
		Waiting **p = &waiting;
		while (*p && (*p)->tag != ev.tag)
			p = &(*p)->next;
		if (*p) {
			Waiting *w = *p;
			*p = w->next;
			if (w->fn)
				w->fn(&ev, w->data);
			free(w);
		}
	}
}

void link_type(const char *data, int n)
{
	if (!link_open())
		return;
	dmc_type(app.s, data, n);
	link_pump();
}

void link_wait(int tag, ResponseFn fn, void *data)
{
	if (tag <= 0)
		return;
	Waiting *w = (Waiting *)calloc(1, sizeof(*w));
	if (!w)
		return;
	w->tag = tag;
	w->fn = fn;
	w->data = data;
	w->next = waiting;
	waiting = w;
	link_pump();
}

int link_command(const char *cmd, int flags, ResponseFn fn, void *data)
{
	int tag = dmc_command(app.s, cmd, flags, 0);
	if (tag > 0) {
		Waiting *w = (Waiting *)calloc(1, sizeof(Waiting));
		w->tag = tag;
		w->fn = fn;
		w->data = data;
		w->next = waiting;
		waiting = w;
		link_pump();
	}
	return tag;
}
