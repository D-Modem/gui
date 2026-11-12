/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * agent.c - the agent libdmodemssh logs in with: on Linux and macOS an
 *           ssh-agent's socket (SSH_AUTH_SOCK, or one given); on
 *           Windows PuTTY's Pageant or OpenSSH's own agent.
 *
 * libssh talks to an agent over a socket it is given, so on Windows a
 * pair of connected sockets stands between it and the agent: a thread
 * reads each of libssh's requests from its end and hands it on, to
 * Pageant by its WM_COPYDATA (a shared memory mapping, as PuTTY's own
 * tools ask it) or down OpenSSH's named pipe, and writes the answer
 * back.  The thread ends when libssh closes its end.
 *
 * This file and agent.h are the same in D-Modem/gui (ssh/) and
 * D-Modem/app (native-ssh/): change both.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <libssh/libssh.h>

#include "agent.h"

#ifdef _WIN32

/* Pageant's: its window, the WM_COPYDATA it answers, its longest message */
#define PAGEANT_ID 0x804e50baUL
#define AGENT_MAX 262144
/* OpenSSH's agent on Windows */
#define OPENSSH_PIPE "\\\\.\\pipe\\openssh-ssh-agent"

struct dmc_agent {
	SOCKET sock;		/* the thread's end */
	HANDLE thread;
	HANDLE pipe;		/* OpenSSH's, or a pipe given; else Pageant */
};

/* a big-endian uint32 at b */
static unsigned long be32(const unsigned char *b)
{
	return (unsigned long)b[0] << 24 | (unsigned long)b[1] << 16 |
	       (unsigned long)b[2] << 8 | b[3];
}

/* Pageant's window, if it's running */
static HWND pageant(void)
{
	return FindWindowA("Pageant", "Pageant");
}

/* the user's SID (malloc'd), or NULL */
static PSID user_sid(void)
{
	HANDLE tok;
	DWORD n = 0;
	TOKEN_USER *tu;
	PSID sid = NULL;

	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
		return NULL;
	GetTokenInformation(tok, TokenUser, NULL, 0, &n);
	tu = n ? malloc(n) : NULL;
	if (tu && GetTokenInformation(tok, TokenUser, tu, n, &n)) {
		DWORD l = GetLengthSid(tu->User.Sid);
		sid = malloc(l);
		if (sid && !CopySid(l, sid, tu->User.Sid)) {
			free(sid);
			sid = NULL;
		}
	}
	free(tu);
	CloseHandle(tok);
	return sid;
}

/* a request (its length first) asked of Pageant; the answer (its length
   first) into rep, which holds AGENT_MAX: 0, or -1 */
static int pageant_query(const unsigned char *req, unsigned long len,
			 unsigned char *rep)
{
	HWND w = pageant();
	char name[64];
	SECURITY_ATTRIBUTES sa, *psa = NULL;
	PSECURITY_DESCRIPTOR sd = NULL;
	PSID sid;
	HANDLE fm;
	unsigned char *p;
	COPYDATASTRUCT cds;
	int r = -1;

	if (!w || len > AGENT_MAX)
		return -1;
	snprintf(name, sizeof name, "PageantRequest%08lx",
		 (unsigned long)GetCurrentThreadId());
	/* (the mapping owned by the user: Pageant answers only its own) */
	sid = user_sid();
	if (sid) {
		sd = LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
		if (sd && InitializeSecurityDescriptor(sd, SECURITY_DESCRIPTOR_REVISION) &&
		    SetSecurityDescriptorOwner(sd, sid, FALSE)) {
			sa.nLength = sizeof sa;
			sa.bInheritHandle = TRUE;
			sa.lpSecurityDescriptor = sd;
			psa = &sa;
		}
	}
	fm = CreateFileMappingA(INVALID_HANDLE_VALUE, psa, PAGE_READWRITE, 0,
				AGENT_MAX, name);
	if (fm) {
		p = MapViewOfFile(fm, FILE_MAP_WRITE, 0, 0, 0);
		if (p) {
			memcpy(p, req, len);
			cds.dwData = PAGEANT_ID;
			cds.cbData = (DWORD)strlen(name) + 1;
			cds.lpData = name;
			if (SendMessageA(w, WM_COPYDATA, 0, (LPARAM)&cds) > 0) {
				unsigned long n = be32(p);
				if (n <= AGENT_MAX - 4) {
					memcpy(rep, p, n + 4);
					r = 0;
				}
			}
			UnmapViewOfFile(p);
		}
		CloseHandle(fm);
	}
	if (sd)
		LocalFree(sd);
	free(sid);
	return r;
}

/* n bytes read from the pipe: 0, or -1 */
static int pipe_read(HANDLE h, unsigned char *b, unsigned long n)
{
	DWORD got;
	while (n) {
		if (!ReadFile(h, b, n, &got, NULL) || !got)
			return -1;
		b += got;
		n -= got;
	}
	return 0;
}

/* a request asked down the agent's pipe; the answer into rep: 0, or -1 */
static int pipe_query(HANDLE h, const unsigned char *req, unsigned long len,
		      unsigned char *rep)
{
	DWORD put;
	unsigned long n;

	while (len) {
		if (!WriteFile(h, req, len, &put, NULL) || !put)
			return -1;
		req += put;
		len -= put;
	}
	if (pipe_read(h, rep, 4))
		return -1;
	n = be32(rep);
	if (n > AGENT_MAX - 4)
		return -1;
	return pipe_read(h, rep + 4, n);
}

/* n bytes from the socket: 0, or -1 at its end */
static int sock_read(SOCKET s, unsigned char *b, unsigned long n)
{
	while (n) {
		int got = recv(s, (char *)b, (int)n, 0);
		if (got <= 0)
			return -1;
		b += got;
		n -= (unsigned long)got;
	}
	return 0;
}

/* the thread: libssh's requests to the agent, its answers back */
static DWORD WINAPI bridge(void *p)
{
	struct dmc_agent *a = p;
	unsigned char *req = malloc(AGENT_MAX), *rep = malloc(AGENT_MAX);
	static const unsigned char failure[5] = { 0, 0, 0, 1, 5 };

	while (req && rep) {
		unsigned long n;
		const unsigned char *out = rep;
		int r, outlen;

		if (sock_read(a->sock, req, 4))
			break;
		n = be32(req);
		if (n > AGENT_MAX - 4 || sock_read(a->sock, req + 4, n))
			break;
		r = a->pipe != INVALID_HANDLE_VALUE ?
		    pipe_query(a->pipe, req, n + 4, rep) :
		    pageant_query(req, n + 4, rep);
		if (r) {
			/* (SSH_AGENT_FAILURE: no agent, or it didn't answer) */
			out = failure;
			outlen = 5;
		} else {
			outlen = (int)be32(rep) + 4;
		}
		if (send(a->sock, (const char *)out, outlen, 0) != outlen)
			break;
	}
	free(req);
	free(rep);
	closesocket(a->sock);
	a->sock = INVALID_SOCKET;
	return 0;
}

/* a pair of connected sockets, by the loopback: 0, or -1 */
static int sock_pair(SOCKET pair[2])
{
	SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP), c, a;
	struct sockaddr_in at, mine, peer;
	int n = sizeof at;

	if (l == INVALID_SOCKET)
		return -1;
	memset(&at, 0, sizeof at);
	at.sin_family = AF_INET;
	at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(l, (struct sockaddr *)&at, sizeof at) || listen(l, 1) ||
	    getsockname(l, (struct sockaddr *)&at, &n)) {
		closesocket(l);
		return -1;
	}
	c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (c == INVALID_SOCKET || connect(c, (struct sockaddr *)&at, sizeof at)) {
		closesocket(l);
		if (c != INVALID_SOCKET)
			closesocket(c);
		return -1;
	}
	n = sizeof peer;
	a = accept(l, (struct sockaddr *)&peer, &n);
	closesocket(l);
	/* (only our own connection: the one from c's own port) */
	n = sizeof mine;
	if (a == INVALID_SOCKET || getsockname(c, (struct sockaddr *)&mine, &n) ||
	    peer.sin_port != mine.sin_port || peer.sin_addr.s_addr != mine.sin_addr.s_addr) {
		if (a != INVALID_SOCKET)
			closesocket(a);
		closesocket(c);
		return -1;
	}
	pair[0] = c;
	pair[1] = a;
	return 0;
}

/* a named pipe opened, or INVALID_HANDLE_VALUE */
static HANDLE pipe_open(const char *path)
{
	return CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
			   OPEN_EXISTING, 0, NULL);
}

/* the agent asked for: "" (Pageant, else OpenSSH's), "pageant", or a pipe */
static int which_pipe(const char *which)
{
	return which && *which && _stricmp(which, "pageant") != 0;
}

/* whether the agent answers (see agent.h) */
int dmc_agent_reachable(const char *which)
{
	HANDLE h;

	if (!which_pipe(which) && pageant())
		return 1;
	if (which && !_stricmp(which, "pageant"))
		return 0;
	h = pipe_open(which_pipe(which) ? which : OPENSSH_PIPE);
	if (h == INVALID_HANDLE_VALUE)
		return 0;
	CloseHandle(h);
	return 1;
}

/* the session's agent set, before its login by the agent (see agent.h) */
int dmc_agent_attach(ssh_session ssh, const char *which, dmc_agent **out)
{
	struct dmc_agent *a;
	SOCKET pair[2];
	HANDLE pipe = INVALID_HANDLE_VALUE;
	DWORD id;

	*out = NULL;
	if (which_pipe(which)) {
		pipe = pipe_open(which);
	} else if (!pageant()) {
		if (which && *which)
			return -1;
		pipe = pipe_open(OPENSSH_PIPE);
	}
	if (pipe == INVALID_HANDLE_VALUE && (which_pipe(which) || !pageant()))
		return -1;
	a = calloc(1, sizeof *a);
	if (!a || sock_pair(pair)) {
		free(a);
		if (pipe != INVALID_HANDLE_VALUE)
			CloseHandle(pipe);
		return -1;
	}
	a->sock = pair[1];
	a->pipe = pipe;
	a->thread = CreateThread(NULL, 0, bridge, a, 0, &id);
	if (!a->thread || ssh_set_agent_socket(ssh, pair[0]) != SSH_OK) {
		closesocket(pair[0]);
		if (a->thread) {
			WaitForSingleObject(a->thread, INFINITE);
			CloseHandle(a->thread);
		} else {
			closesocket(pair[1]);
		}
		if (pipe != INVALID_HANDLE_VALUE)
			CloseHandle(pipe);
		free(a);
		return -1;
	}
	*out = a;
	return 0;
}

/* what dmc_agent_attach() made, freed after its session */
void dmc_agent_detach(dmc_agent *a)
{
	if (!a)
		return;
	/* (libssh closed its end with the session: the thread has ended or
	   ends now) */
	WaitForSingleObject(a->thread, INFINITE);
	CloseHandle(a->thread);
	if (a->pipe != INVALID_HANDLE_VALUE)
		CloseHandle(a->pipe);
	free(a);
}

#else

/* whether the agent answers (see agent.h) */
int dmc_agent_reachable(const char *which)
{
	const char *path = which && *which ? which : getenv("SSH_AUTH_SOCK");
	char full[sizeof(((struct sockaddr_un *)0)->sun_path)];
	struct sockaddr_un sun;
	int fd, ok;

	if (!path || !*path)
		return 0;
	/* (~ the home folder, as OpenSSH's IdentityAgent has it) */
	if (path[0] == '~' && path[1] == '/' && getenv("HOME"))
		snprintf(full, sizeof full, "%s%s", getenv("HOME"), path + 1);
	else
		snprintf(full, sizeof full, "%s", path);
	memset(&sun, 0, sizeof sun);
	sun.sun_family = AF_UNIX;
	memcpy(sun.sun_path, full, sizeof sun.sun_path - 1);
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return 0;
	ok = connect(fd, (struct sockaddr *)&sun, sizeof sun) == 0;
	close(fd);
	return ok;
}

/* the session's agent set, before its login by the agent (see agent.h) */
int dmc_agent_attach(ssh_session ssh, const char *which, dmc_agent **out)
{
	*out = NULL;
	/* (libssh connects to it itself; none given: SSH_AUTH_SOCK) */
	if (which && *which)
		return ssh_options_set(ssh, SSH_OPTIONS_IDENTITY_AGENT, which) == SSH_OK ? 0 : -1;
	return 0;
}

/* what dmc_agent_attach() made, freed after its session */
void dmc_agent_detach(dmc_agent *a)
{
	(void)a;
}

#endif
