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
 *
 * With the modem built in (DMODEM_GUI_EMBED: Windows 98 SE and later,
 * and the Windows app on Linux and macOS),
 * the dmodemd line starts no dmodemd: libdmodem_embed runs the modem in
 * a thread of the app, and gives a connected socket as its DTE.  The
 * thread's log lines and its end come to the main window as
 * WM_APP_EMBED.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"

#ifdef DMODEM_GUI_EMBED
#include "dmodem_embed.h"
#endif

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
static int connect_tries;
#ifdef DMODEM_GUI_EMBED
static dmodem_embed *embed;             /* the modem built in */
static int embed_running;
static unsigned embed_gen;              /* its start's number */
#endif

static int64_t now_ms()
{
	return (int64_t)GetTickCount();
}

bool link_open()
{
	return (sock != INVALID_SOCKET && sock_ready) ||
		com != INVALID_HANDLE_VALUE;
}

bool link_break()
{
	/* (a COM port's: the line held at space for 250 ms) */
	if (com == INVALID_HANDLE_VALUE || !SetCommBreak(com))
		return false;
	Sleep(250);
	ClearCommBreak(com);
	return true;
}

bool link_running()
{
#ifdef DMODEM_GUI_EMBED
	if (embed_running)
		return true;
#endif
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
	/* (a device path on Linux and macOS, winport/: as it is) */
	char path[96];
	if (name[0] == '/')
		_snprintf(path, sizeof(path), "%s", name);
	else
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
	/* Line Settings' bits per second, data bits, parity, stop bits and
	   flow control (Xon/Xoff: DC1 and DC3, both ways; hardware: RTS and
	   CTS) */
	dcb.BaudRate = (DWORD)app.cfg.com_bps;
	dcb.ByteSize = (BYTE)app.cfg.com_data;
	dcb.Parity = (BYTE)app.cfg.com_parity;
	dcb.fParity = app.cfg.com_parity != 0;
	dcb.StopBits = (BYTE)app.cfg.com_stop;
	dcb.fBinary = TRUE;
	dcb.fDtrControl = DTR_CONTROL_ENABLE;
	dcb.fRtsControl = app.cfg.com_flow == 2 ? RTS_CONTROL_HANDSHAKE : 0x01;
	dcb.fOutxCtsFlow = app.cfg.com_flow == 2;
	dcb.fOutX = dcb.fInX = app.cfg.com_flow == 1;
	dcb.XonChar = 0x11;
	dcb.XoffChar = 0x13;
	dcb.XonLim = 256;
	dcb.XoffLim = 256;
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

/* dmodemd: the configured one, or next to this app */
static void dmodemd_path(char *out, int n)
{
	if (app.cfg.dmodemd[0]) {
		_snprintf(out, n, "%s", app.cfg.dmodemd);
		out[n - 1] = '\0';
		return;
	}
	GetModuleFileName(NULL, out, n);
	char *slash = strrchr(out, DIR_SEP[0]);
	if (slash)
		strcpy(slash + 1, DMODEMD_EXE);
#ifdef DMODEM_WINPORT
	/* (Linux and macOS, winport/: else the one on the PATH) */
	char found[MAX_PATH];
	if (GetFileAttributes(out) == (DWORD)-1 &&
	    SearchPath(NULL, DMODEMD_EXE, NULL, sizeof(found), found, NULL))
		_snprintf(out, n, "%s", found);
	out[n - 1] = '\0';
#endif
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
	if (!c.usb[0] && !config_sip_ready(&c)) {
		app_message("Set up the SIP account first (Modem, Line Settings)");
		return false;
	}
	char exe[MAX_PATH];
	dmodemd_path(exe, sizeof(exe));
	if (GetFileAttributes(exe) == (DWORD)-1) {
		char m[MAX_PATH + 64];
		_snprintf(m, sizeof(m), DMODEMD_EXE " was not found (%s)", exe);
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
	/* (no account: dmodemd's anonymous user agent) */
	if (c.sip_anonymous)
		_snprintf(login, sizeof(login), "SIP_LOGIN=anonymous");
	else
		_snprintf(login, sizeof(login), "SIP_LOGIN=%s:%s@%s",
			  c.sip_user, c.sip_password, c.sip_server);
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
	connect_tries = 0;
	return true;
}

#ifdef DMODEM_GUI_EMBED
/* news from the modem's thread (WM_APP_EMBED's lParam): a log line, or
   the end (line empty) with its status */
struct EmbedNews {
	unsigned gen;
	int end, status;
	char line[1];
};

/* a log line, or the end, posted to the main window (any thread) */
static void embed_post(unsigned gen, const char *line, int end, int status)
{
	size_t n = line ? strlen(line) : 0;
	EmbedNews *e = (EmbedNews *)malloc(sizeof(EmbedNews) + n);
	if (!e)
		return;
	e->gen = gen;
	e->end = end;
	e->status = status;
	memcpy(e->line, line ? line : "", n + 1);
	if (!PostMessage(app.main, WM_APP_EMBED, 0, (LPARAM)e))
		free(e);
}

/* a line of the modem's log (the modem's thread); ctx is the start's
   number */
static void embed_log(void *ctx, const char *line)
{
	embed_post((unsigned)(UINT_PTR)ctx, line, 0, 0);
}

/* the modem stopped (the modem's thread) */
static void embed_exit(void *ctx, int status)
{
	embed_post((unsigned)(UINT_PTR)ctx, NULL, 1, status);
}

/* the modem started in the app on the line settings' line, its DTE the
   socket that libdmodem_embed gives */
static bool start_embedded()
{
	Config &c = app.cfg;
	if (!c.usb[0] && !config_sip_ready(&c)) {
		app_message("Set up the SIP account first (Modem, Line Settings)");
		return false;
	}
	dmodem_embed_config e;
	dmodem_embed_config_init(&e);
	e.country = c.country;
	if (c.usb[0]) {
		e.line = DMODEM_EMBED_USB;
		e.usb = c.usb;
	} else {
		e.line = DMODEM_EMBED_SIP;
		if (!c.sip_anonymous) {
			e.sip_user = c.sip_user;
			e.sip_password = c.sip_password;
			e.sip_domain = c.sip_server;
		}
		e.sip_listen = c.listen;
		e.sip_t38 = c.t38;
		e.sip_v152 = c.v152;
		e.sip_v150 = c.v150;
	}
	/* the line's measurements, kept from call to call */
	char data[MAX_PATH];
	app_data_dir(data, sizeof(data) - 16);
	/* (the library opens it with the C library: "/" on Linux and macOS) */
	strcat(data, DIR_SEP "dmodem0.data");
	e.data_file = data;
	if (!embed)
		embed = dmodem_embed_new();
	if (!embed)
		return false;
	embed_gen++;
	dmodem_embed_on_log(embed, embed_log, (void *)(UINT_PTR)embed_gen);
	dmodem_embed_on_exit(embed, embed_exit, (void *)(UINT_PTR)embed_gen);
	int fd = dmodem_embed_start(embed, &e);
	char m[300];
	if (fd < 0) {
		_snprintf(m, sizeof(m), "Can't start the built-in modem: %s",
			  dmodem_embed_error(embed));
		m[sizeof(m) - 1] = '\0';
		app_message(m);
		app_log(m);
		return false;
	}
	_snprintf(m, sizeof(m), "The built-in modem (D-Modem %s) started.",
		  dmodem_embed_version());
	m[sizeof(m) - 1] = '\0';
	app_log(m);
	embed_running = 1;
	/* (a connected socket: the same messages as a TCP line's) */
	sock = (SOCKET)fd;
	sock_ready = 1;
	WSAAsyncSelect(sock, app.main, WM_APP_SOCKET,
		       FD_READ | FD_WRITE | FD_CLOSE);
	opened("Built-in modem");
	return true;
}

/* the modem built in stopped, its thread ended */
static void stop_embedded()
{
	if (!embed || !embed_running)
		return;
	/* (news of this start after now is not wanted) */
	embed_gen++;
	dmodem_embed_stop(embed);
	embed_running = 0;
	app_log("The built-in modem stopped.");
}
#endif

/* see app.h */
void link_embed(WPARAM w, LPARAM l)
{
	(void)w;
#ifdef DMODEM_GUI_EMBED
	EmbedNews *e = (EmbedNews *)l;
	if (e && e->gen == embed_gen) {
		if (!e->end) {
			app_log(e->line);
			if (dmc_daemon_error(e->line) && !link_open())
				app_message(e->line + 7);
		} else {
			const char *m = e->status < 0 ?
				"The built-in modem stopped (error)." :
				"The built-in modem stopped.";
			app_log(m);
			if (e->status < 0)
				app_message(m);
			embed_running = 0;
			close_port();
			app_state();
		}
	}
	free(e);
#else
	(void)l;
#endif
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
#ifdef DMODEM_GUI_EMBED
		if (app.cfg.builtin) {
			ok = start_embedded();
			break;
		}
#endif
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
#ifdef DMODEM_GUI_EMBED
	stop_embedded();
#endif
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
	/* (what the DTE can't take yet waits in the session) */
	for (;;) {
		int room = (int)sizeof(port_out) - port_out_n;
		if (room > (int)sizeof(buf))
			room = (int)sizeof(buf);
		if (room <= 0 || (n = dmc_output(app.s, buf, room)) <= 0)
			break;
		memcpy(port_out + port_out_n, buf, n);
		port_out_n += n;
		port_write();
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

void link_send(const char *data, int n)
{
	if (!link_open())
		return;
	dmc_send(app.s, data, n);
	link_pump();
}

int link_room()
{
	if (!link_open())
		return 0;
	int room = (int)sizeof(port_out) - port_out_n - dmc_output_waiting(app.s);
	return room > 0 ? room : 0;
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
