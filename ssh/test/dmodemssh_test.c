/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * dmodemssh_test.c - libdmodemssh's tests: keys (always), and a login to
 * a real sshd when DMODEM_SSH_TEST names one as host:port:user:password
 * (a shell, a command, a direct channel and a remote listen on it).
 * DMODEM_SSH_TEST_KEY, a private key's file that the user's
 * authorized_keys has, adds a login by key.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define closesocket close
#endif

#include "dmodemssh.h"
#include "ppk.h"

static int failures;

#define CHECK(c) do { if (!(c)) { \
	fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #c); \
	failures++; } } while (0)

/* a key made, read back, its fingerprint, and with a passphrase */
static void test_keys(void)
{
	static const char *types[] = { "ed25519", "ecdsa", "rsa" };
	static const int bits[] = { 0, 384, 2048 };
	int i;

	for (i = 0; i < 3; i++) {
		char *priv = NULL, *pub = NULL, *pub2 = NULL, *enc = NULL;
		char fp[128];

		CHECK(dmc_ssh_keygen(types[i], bits[i], NULL, &priv, &pub) == 0);
		if (!priv || !pub)
			continue;
		CHECK(strstr(priv, "BEGIN OPENSSH PRIVATE KEY") != NULL);
		CHECK(dmc_ssh_key_public(priv, NULL, &pub2) == 0);
		CHECK(pub2 && !strcmp(pub, pub2));
		CHECK(dmc_ssh_fingerprint(pub, fp, sizeof fp) == 0);
		CHECK(!strncmp(fp, "SHA256:", 7) && strlen(fp) == 50);
		CHECK(dmc_ssh_key_encrypted(priv) == 0);
		CHECK(dmc_ssh_key_export(priv, NULL, "sesame", &enc) == 0);
		CHECK(enc && dmc_ssh_key_encrypted(enc) == 1);
		dmc_ssh_free_string(pub2);
		pub2 = NULL;
		CHECK(enc && dmc_ssh_key_public(enc, "wrong", &pub2) < 0);
		CHECK(enc && dmc_ssh_key_public(enc, "sesame", &pub2) == 0);
		CHECK(pub2 && !strcmp(pub, pub2));
		dmc_ssh_free_string(priv);
		dmc_ssh_free_string(pub);
		dmc_ssh_free_string(pub2);
		dmc_ssh_free_string(enc);
	}
	{
		char fp[128];
		CHECK(dmc_ssh_fingerprint("ssh-ed25519 notbase64!", fp, sizeof fp) < 0);
	}
}

/* PuTTY's keys (made by PuTTYgen 0.81, for these tests only): version 3
   with Argon2id, version 2, and one with no passphrase */
static const char ppk_v3[] =
	"PuTTY-User-Key-File-3: ssh-ed25519\n"
	"Encryption: aes256-cbc\n"
	"Comment: dmodem test\n"
	"Public-Lines: 2\n"
	"AAAAC3NzaC1lZDI1NTE5AAAAIGaq1WGILuZaAUWi0Oj7RUOfHb2IdFdPFkpBvgmT\n"
	"FNDY\n"
	"Key-Derivation: Argon2id\n"
	"Argon2-Memory: 1024\n"
	"Argon2-Passes: 2\n"
	"Argon2-Parallelism: 1\n"
	"Argon2-Salt: 33f9ba393a4951fb796e2fbc41661b75\n"
	"Private-Lines: 1\n"
	"hM2XhuziydvZHlIY7Kv3LLpDLxZpyZRk0FUEdY9QffTXspVHngnD3kDEYfdjBqsL\n"
	"Private-MAC: 335d60073336e5ef6af9660f801a38edc92fc04f52d7042d9a598a180787d6b6\n";
static const char ppk_v3_pub[] =
	"ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIGaq1WGILuZaAUWi0Oj7RUOfHb2IdFdPFkpBvgmTFNDY";
static const char ppk_v2[] =
	"PuTTY-User-Key-File-2: ecdsa-sha2-nistp256\n"
	"Encryption: aes256-cbc\n"
	"Comment: dmodem test\n"
	"Public-Lines: 3\n"
	"AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBP8cgz/JGtuf\n"
	"pTcbwYWaC5rRxEtd94wTxyYeZmpibjtHZs0xe5R/U0/gsjoYAZBqLsVAuw/m7PEH\n"
	"owsid94C7vQ=\n"
	"Private-Lines: 1\n"
	"tXUEPEl6phjZUecV/5HDrEjp64lDdujz0/2vShR6B1ayoimLGtze26jtLPWczTOn\n"
	"Private-MAC: 9ef17c9fa24457b60b4cc20f2f8e07925a9ec090\n";
static const char ppk_v2_pub[] =
	"ecdsa-sha2-nistp256 AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBP8cgz/JGtufpTcbwYWaC5rRxEtd94wTxyYeZmpibjtHZs0xe5R/U0/gsjoYAZBqLsVAuw/m7PEHowsid94C7vQ=";
static const char ppk_plain[] =
	"PuTTY-User-Key-File-3: ssh-ed25519\n"
	"Encryption: none\n"
	"Comment: dmodem test\n"
	"Public-Lines: 2\n"
	"AAAAC3NzaC1lZDI1NTE5AAAAIJuvvweC5NNfYlxOXbIN7mOvU3s9uMr4WbpWTlzY\n"
	"bOOZ\n"
	"Private-Lines: 1\n"
	"AAAAID9RN2flxRLWco4mJwxMJJQUNSmAEZ4ltTevZi+TSuxO\n"
	"Private-MAC: c31121241789d04ca3e7e68d528a5c46a4309276510c45cdc6b4e0f501746f2f\n";
static const char ppk_plain_pub[] =
	"ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIJuvvweC5NNfYlxOXbIN7mOvU3s9uMr4WbpWTlzYbOOZ";

/* Argon2d, Argon2i and Argon2id: RFC 9106's test vectors */
static void test_argon2(void)
{
	static const char *want[3] = {
		"512b391b6f1162975371d30919734294f868e3be3984f3c1a13a4db9fabe4acb",
		"c814d9d1dc7f37aa13f0d77f2494bda1c8de6b016dd388d29952a4c4672b6ce8",
		"0d640df58d78766c08c037a34a8b53c9d01ef0452d75b65eb52520e96b01e659",
	};
	unsigned char pwd[32], salt[16], secret[8], ad[12], out[32];
	char hex[65];
	int t, i;

	memset(pwd, 1, sizeof pwd);
	memset(salt, 2, sizeof salt);
	memset(secret, 3, sizeof secret);
	memset(ad, 4, sizeof ad);
	for (t = 0; t < 3; t++) {
		CHECK(dmc_argon2(t, 32, 3, 4, pwd, sizeof pwd, salt, sizeof salt,
				 secret, sizeof secret, ad, sizeof ad, out, 32) == 0);
		for (i = 0; i < 32; i++)
			snprintf(hex + 2 * i, 3, "%02x", out[i]);
		CHECK(!strcmp(hex, want[t]));
	}
}

/* PuTTY's .ppk read: its public line, a wrong passphrase refused, and
   OpenSSH's form written from it */
static void test_ppk(void)
{
	static const struct { const char *ppk, *pub, *pass; } k[] = {
		{ ppk_v3, ppk_v3_pub, "sesame" },
		{ ppk_v2, ppk_v2_pub, "sesame" },
		{ ppk_plain, ppk_plain_pub, NULL },
	};
	int i;

	for (i = 0; i < 3; i++) {
		char *pub = NULL, *out = NULL, *pub2 = NULL;

		CHECK(dmc_ssh_key_encrypted(k[i].ppk) == (k[i].pass != NULL));
		CHECK(dmc_ssh_key_public(k[i].ppk, k[i].pass, &pub) == 0);
		CHECK(pub && !strcmp(pub, k[i].pub));
		if (k[i].pass) {
			char *no = NULL;
			CHECK(dmc_ssh_key_public(k[i].ppk, "wrong", &no) < 0);
			CHECK(dmc_ssh_key_public(k[i].ppk, NULL, &no) < 0);
		}
		CHECK(dmc_ssh_key_export(k[i].ppk, k[i].pass, "new", &out) == 0);
		CHECK(out && strstr(out, "BEGIN OPENSSH PRIVATE KEY") != NULL);
		CHECK(out && dmc_ssh_key_encrypted(out) == 1);
		CHECK(out && dmc_ssh_key_public(out, "new", &pub2) == 0);
		CHECK(pub2 && !strcmp(pub2, k[i].pub));
		dmc_ssh_free_string(pub);
		dmc_ssh_free_string(pub2);
		dmc_ssh_free_string(out);
	}
	CHECK(dmc_ssh_key_encrypted("not a key") == 0);
}

static const char *host, *user, *password;
static int port;

/* the next event (10 s at most): its type, or 0 */
static int next(dmc_ssh *s, dmc_ssh_event *ev)
{
	int r = dmc_ssh_poll(s, 10000, ev);

	if (r <= 0) {
		fprintf(stderr, "no event (%d)\n", r);
		return 0;
	}
	return ev->type;
}

/* a connection logged in by password (a wrong one first): or NULL */
static dmc_ssh *logged_in(void)
{
	dmc_ssh *s = dmc_ssh_new(host, port, user);
	dmc_ssh_event ev;

	CHECK(next(s, &ev) == DMC_SSH_EV_HOSTKEY);
	CHECK(!strncmp(ev.data, "ssh-", 4) || !strncmp(ev.data, "ecdsa-", 6));
	CHECK(ev.value == DMC_SSH_HOST_UNCHECKED);
	dmc_ssh_trust(s, 1, 0);
	CHECK(next(s, &ev) == DMC_SSH_EV_AUTH);
	CHECK(ev.value & DMC_SSH_AUTH_PASSWORD);
	CHECK(!strcmp(ev.data, ""));
	dmc_ssh_auth_password(s, "not it");
	CHECK(next(s, &ev) == DMC_SSH_EV_AUTH);
	CHECK(!strcmp(ev.data, "The password was not accepted"));
	dmc_ssh_auth_password(s, password);
	if (next(s, &ev) != DMC_SSH_EV_READY) {
		CHECK(!"logged in");
		dmc_ssh_free(s);
		return NULL;
	}
	return s;
}

/* a channel's output read until it closes: into out; its exit status */
static int drain(dmc_ssh *s, int ch, char *out, int max)
{
	dmc_ssh_event ev;
	int len = 0, status = -1000;

	for (;;) {
		int t = next(s, &ev);

		if (!t || t == DMC_SSH_EV_CLOSED)
			break;
		if (ev.channel != ch)
			continue;
		if (t == DMC_SSH_EV_DATA && len + ev.len < max) {
			memcpy(out + len, ev.data, (size_t)ev.len);
			len += ev.len;
		} else if (t == DMC_SSH_EV_EXIT) {
			status = ev.value;
		} else if (t == DMC_SSH_EV_CLOSE) {
			break;
		}
	}
	out[len] = 0;
	return status;
}

/* a shell on a pty: its size, a command typed, its exit */
static void test_shell(dmc_ssh *s)
{
	dmc_ssh_event ev;
	char out[8192];
	int ch = dmc_ssh_shell(s, "vt100", 100, 30, 0);
	static const char typed[] = "stty size; echo $TERM; exit 3\n";

	CHECK(ch > 0);
	CHECK(next(s, &ev) == DMC_SSH_EV_OPEN && ev.channel == ch && ev.value == 1);
	CHECK(dmc_ssh_write(s, ch, typed, (int)strlen(typed)) >= 0);
	CHECK(drain(s, ch, out, sizeof out) == 3);
	CHECK(strstr(out, "30 100") != NULL);
	CHECK(strstr(out, "vt100") != NULL);
	CHECK(dmc_ssh_write(s, ch, "x", 1) < 0);
}

/* a command: its output, stdin, and exit status */
static void test_exec(dmc_ssh *s)
{
	dmc_ssh_event ev;
	char out[8192];
	int ch = dmc_ssh_exec(s, "tr a-z A-Z; exit 7", 0);

	CHECK(ch > 0);
	CHECK(next(s, &ev) == DMC_SSH_EV_OPEN && ev.value == 1);
	CHECK(dmc_ssh_write(s, ch, "hello\n", 6) >= 0);
	dmc_ssh_eof(s, ch);
	CHECK(drain(s, ch, out, sizeof out) == 7);
	CHECK(!strcmp(out, "HELLO\n"));
}

/* a direct channel to the sshd itself: its banner */
static void test_direct(dmc_ssh *s)
{
	dmc_ssh_event ev;
	int ch = dmc_ssh_direct(s, "127.0.0.1", port, "127.0.0.1", 40000);
	int t;

	CHECK(ch > 0);
	CHECK(next(s, &ev) == DMC_SSH_EV_OPEN && ev.value == 1);
	do {
		t = next(s, &ev);
	} while (t && t != DMC_SSH_EV_DATA);
	CHECK(t == DMC_SSH_EV_DATA && !strncmp(ev.data, "SSH-2.0-", 8));
	dmc_ssh_close(s, ch);
	CHECK(next(s, &ev) == DMC_SSH_EV_CLOSE && ev.channel == ch);
}

/* a remote listen on a port the server picks, connected to from here */
static void test_listen(dmc_ssh *s)
{
	dmc_ssh_event ev;
	int id = dmc_ssh_listen(s, "127.0.0.1", 0), bound, ch, t;
	struct sockaddr_in a;
	int fd;
	char b[16];

	CHECK(id > 0);
	CHECK(next(s, &ev) == DMC_SSH_EV_LISTEN && ev.channel == id);
	bound = ev.value;
	CHECK(bound > 0);
	if (bound <= 0)
		return;
	fd = (int)socket(AF_INET, SOCK_STREAM, 0);
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons((unsigned short)bound);
	a.sin_addr.s_addr = htonl(0x7f000001);
	CHECK(connect(fd, (struct sockaddr *)&a, sizeof a) == 0);
	send(fd, "ping", 4, 0);
	CHECK(next(s, &ev) == DMC_SSH_EV_INCOMING && ev.value == bound);
	ch = ev.channel;
	do {
		t = next(s, &ev);
	} while (t && t != DMC_SSH_EV_DATA);
	CHECK(t == DMC_SSH_EV_DATA && ev.channel == ch && ev.len == 4 &&
	      !memcmp(ev.data, "ping", 4));
	CHECK(dmc_ssh_write(s, ch, "pong", 4) >= 0);
	{
		/* (the reply comes as the session is polled) */
		int got = 0, i;
		for (i = 0; i < 40 && got < 4; i++) {
			dmc_ssh_poll(s, 50, &ev);
#ifdef _WIN32
			{
				u_long n = 0;
				ioctlsocket(fd, FIONREAD, &n);
				if (n >= 4)
					got = recv(fd, b, 4, 0);
			}
#else
			got = (int)recv(fd, b, 4, MSG_DONTWAIT);
			if (got < 0)
				got = 0;
#endif
		}
		CHECK(got == 4 && !memcmp(b, "pong", 4));
	}
	closesocket(fd);
	dmc_ssh_close(s, ch);
	dmc_ssh_unlisten(s, "127.0.0.1", bound);
}

/* a login by a key in a file */
static void test_key_login(const char *file)
{
	FILE *f = fopen(file, "rb");
	char key[16384];
	size_t n;
	dmc_ssh *s;
	dmc_ssh_event ev;

	CHECK(f != NULL);
	if (!f)
		return;
	n = fread(key, 1, sizeof key - 1, f);
	key[n] = 0;
	fclose(f);
	s = dmc_ssh_new(host, port, user);
	CHECK(next(s, &ev) == DMC_SSH_EV_HOSTKEY);
	dmc_ssh_trust(s, 1, 0);
	CHECK(next(s, &ev) == DMC_SSH_EV_AUTH && (ev.value & DMC_SSH_AUTH_PUBLICKEY));
	CHECK(dmc_ssh_auth_key(s, key, NULL) == 0);
	CHECK(next(s, &ev) == DMC_SSH_EV_READY);
	dmc_ssh_free(s);
}

/* a login by the agent named (a socket, or on Windows Pageant or a
   pipe) */
static void test_agent_login(const char *agent)
{
	dmc_ssh *s;
	dmc_ssh_event ev;

	CHECK(dmc_ssh_agent_reachable(agent) == 1);
	s = dmc_ssh_new(host, port, user);
	CHECK(dmc_ssh_option(s, "agent", agent) == 0);
	CHECK(next(s, &ev) == DMC_SSH_EV_HOSTKEY);
	dmc_ssh_trust(s, 1, 0);
	CHECK(next(s, &ev) == DMC_SSH_EV_AUTH && (ev.value & DMC_SSH_AUTH_PUBLICKEY));
	dmc_ssh_auth_agent(s);
	CHECK(next(s, &ev) == DMC_SSH_EV_READY);
	dmc_ssh_free(s);
}

#ifdef _WIN32
/*
 * A stand-in for Pageant: its window, answering by WM_COPYDATA as it
 * does, with no keys (each request counted), so that the way to it is
 * tested where PuTTY isn't.
 */
static int pageant_requests;

/* the stand-in's window: a request in the named mapping, answered there */
static LRESULT CALLBACK pageant_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_COPYDATA) {
		COPYDATASTRUCT *cds = (COPYDATASTRUCT *)lp;
		HANDLE fm;
		unsigned char *p;

		if (cds->dwData != 0x804e50baUL)
			return 0;
		fm = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, (const char *)cds->lpData);
		if (!fm)
			return 0;
		p = MapViewOfFile(fm, FILE_MAP_WRITE, 0, 0, 0);
		if (p) {
			pageant_requests++;
			if (p[4] == 11) {
				/* SSH2_AGENT_IDENTITIES_ANSWER, none */
				static const unsigned char none[9] = { 0, 0, 0, 5, 12, 0, 0, 0, 0 };
				memcpy(p, none, sizeof none);
			} else {
				/* SSH_AGENT_FAILURE */
				static const unsigned char fail[5] = { 0, 0, 0, 1, 5 };
				memcpy(p, fail, sizeof fail);
			}
			UnmapViewOfFile(p);
		}
		CloseHandle(fm);
		return 1;
	}
	return DefWindowProcA(w, msg, wp, lp);
}

/* the stand-in's thread: its window and their messages */
static DWORD WINAPI pageant_run(void *ready)
{
	WNDCLASSA wc;
	MSG m;

	memset(&wc, 0, sizeof wc);
	wc.lpfnWndProc = pageant_proc;
	wc.hInstance = GetModuleHandleA(NULL);
	wc.lpszClassName = "Pageant";
	RegisterClassA(&wc);
	CreateWindowA("Pageant", "Pageant", 0, 0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
	SetEvent((HANDLE)ready);
	while (GetMessageA(&m, NULL, 0, 0) > 0)
		DispatchMessageA(&m);
	return 0;
}

/* Pageant found, asked, and its "no keys" taken as a login refused */
static void test_pageant(void)
{
	HANDLE ready = CreateEventA(NULL, TRUE, FALSE, NULL);
	DWORD id;
	dmc_ssh *s;
	dmc_ssh_event ev;

	if (FindWindowA("Pageant", "Pageant")) {
		printf("Pageant is running: its stand-in not tested\n");
		return;
	}
	CHECK(dmc_ssh_agent_reachable("pageant") == 0);
	CreateThread(NULL, 0, pageant_run, ready, 0, &id);
	WaitForSingleObject(ready, 5000);
	if (!FindWindowA("Pageant", "Pageant")) {
		printf("no window could be made: Pageant not tested\n");
		return;
	}
	CHECK(dmc_ssh_agent_reachable("pageant") == 1);
	CHECK(dmc_ssh_agent_reachable("") == 1);
	if (!host)
		return;
	s = dmc_ssh_new(host, port, user);
	CHECK(dmc_ssh_option(s, "agent", "pageant") == 0);
	CHECK(next(s, &ev) == DMC_SSH_EV_HOSTKEY);
	dmc_ssh_trust(s, 1, 0);
	CHECK(next(s, &ev) == DMC_SSH_EV_AUTH);
	dmc_ssh_auth_agent(s);
	CHECK(next(s, &ev) == DMC_SSH_EV_AUTH);
	CHECK(pageant_requests >= 1);
	dmc_ssh_free(s);
}
/* a stand-in for OpenSSH's agent on its named pipe: no keys, as above */
#define TEST_PIPE "\\\\.\\pipe\\dmodem-test-agent"
static int pipe_requests;

/* the pipe's one connection served, until it closes */
static DWORD WINAPI pipe_run(void *h)
{
	unsigned char req[1024];
	DWORD n;

	if (!ConnectNamedPipe((HANDLE)h, NULL) && GetLastError() != ERROR_PIPE_CONNECTED)
		return 0;
	while (ReadFile((HANDLE)h, req, sizeof req, &n, NULL) && n >= 5) {
		static const unsigned char none[9] = { 0, 0, 0, 5, 12, 0, 0, 0, 0 };
		static const unsigned char fail[5] = { 0, 0, 0, 1, 5 };
		pipe_requests++;
		if (req[4] == 11)
			WriteFile((HANDLE)h, none, sizeof none, &n, NULL);
		else
			WriteFile((HANDLE)h, fail, sizeof fail, &n, NULL);
	}
	CloseHandle((HANDLE)h);
	return 0;
}

/* an agent on a named pipe found and asked */
static void test_agent_pipe(void)
{
	HANDLE h;
	DWORD id;
	dmc_ssh *s;
	dmc_ssh_event ev;

	CHECK(dmc_ssh_agent_reachable(TEST_PIPE) == 0);
	if (!host)
		return;
	h = CreateNamedPipeA(TEST_PIPE, PIPE_ACCESS_DUPLEX,
			     PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1,
			     4096, 4096, 0, NULL);
	CHECK(h != INVALID_HANDLE_VALUE);
	if (h == INVALID_HANDLE_VALUE)
		return;
	CreateThread(NULL, 0, pipe_run, h, 0, &id);
	s = dmc_ssh_new(host, port, user);
	CHECK(dmc_ssh_option(s, "agent", TEST_PIPE) == 0);
	CHECK(next(s, &ev) == DMC_SSH_EV_HOSTKEY);
	dmc_ssh_trust(s, 1, 0);
	CHECK(next(s, &ev) == DMC_SSH_EV_AUTH);
	dmc_ssh_auth_agent(s);
	CHECK(next(s, &ev) == DMC_SSH_EV_AUTH);
	CHECK(pipe_requests >= 1);
	dmc_ssh_free(s);
}
#endif

/* the logins and channels against DMODEM_SSH_TEST's sshd */
static void test_live(const char *spec)
{
	static char buf[512];
	char *p[4];
	int i;
	dmc_ssh *s;
	dmc_ssh_event ev;
	/* (a key's file, OpenSSH's; a .ppk; an agent's socket) */
	const char *key = getenv("DMODEM_SSH_TEST_KEY");

	snprintf(buf, sizeof buf, "%s", spec);
	p[0] = buf;
	for (i = 1; i < 4; i++) {
		p[i] = strchr(p[i - 1], ':');
		if (!p[i]) {
			fprintf(stderr, "DMODEM_SSH_TEST: host:port:user:password\n");
			failures++;
			return;
		}
		*p[i]++ = 0;
	}
	host = p[0];
	port = atoi(p[1]);
	user = p[2];
	password = p[3];

	s = logged_in();
	if (!s)
		return;
	test_shell(s);
	test_exec(s);
	test_direct(s);
	test_listen(s);
	dmc_ssh_disconnect(s);
	while (next(s, &ev) && ev.type != DMC_SSH_EV_CLOSED)
		;
	CHECK(ev.type == DMC_SSH_EV_CLOSED && ev.data == NULL);
	CHECK(dmc_ssh_poll(s, 10, &ev) == -1);
	dmc_ssh_free(s);

	/* a refused key: the connection ends saying so */
	s = dmc_ssh_new(host, port, user);
	CHECK(next(s, &ev) == DMC_SSH_EV_HOSTKEY);
	dmc_ssh_trust(s, 0, 0);
	CHECK(next(s, &ev) == DMC_SSH_EV_CLOSED);
	CHECK(ev.data && !strcmp(ev.data, "the host's key was not accepted"));
	dmc_ssh_free(s);

	/* keyboard-interactive, when the server has it: the password asked */
	s = dmc_ssh_new(host, port, user);
	CHECK(next(s, &ev) == DMC_SSH_EV_HOSTKEY);
	dmc_ssh_trust(s, 1, 0);
	CHECK(next(s, &ev) == DMC_SSH_EV_AUTH);
	if (ev.value & DMC_SSH_AUTH_INTERACTIVE) {
		int echo = 1;

		dmc_ssh_auth_interactive(s);
		CHECK(next(s, &ev) == DMC_SSH_EV_PROMPTS && ev.value == 1);
		CHECK(strstr(dmc_ssh_prompt(s, 0, &echo), "assword") != NULL);
		CHECK(echo == 0);
		dmc_ssh_answer(s, 0, password);
		dmc_ssh_answered(s);
		/* (some servers end with a round of no questions) */
		while (next(s, &ev) == DMC_SSH_EV_PROMPTS && ev.value == 0)
			dmc_ssh_answered(s);
		CHECK(ev.type == DMC_SSH_EV_READY);
	} else {
		printf("no keyboard-interactive: not tested\n");
	}
	dmc_ssh_free(s);

	if (key)
		test_key_login(key);
	if (getenv("DMODEM_SSH_TEST_PPK"))
		test_key_login(getenv("DMODEM_SSH_TEST_PPK"));
	if (getenv("DMODEM_SSH_TEST_AGENT"))
		test_agent_login(getenv("DMODEM_SSH_TEST_AGENT"));
	CHECK(dmc_ssh_agent_reachable("/nonexistent/agent.sock") == 0);
}

/* the key tests, then the live ones when DMODEM_SSH_TEST names a server */
int main(void)
{
	const char *spec = getenv("DMODEM_SSH_TEST");

#ifdef _WIN32
	WSADATA w;
	WSAStartup(MAKEWORD(2, 2), &w);
#endif
	test_keys();
	test_argon2();
	test_ppk();
	if (spec)
		test_live(spec);
	else
		printf("DMODEM_SSH_TEST is not set: no login tested\n");
#ifdef _WIN32
	test_pageant();
	test_agent_pipe();
#endif
	if (failures) {
		fprintf(stderr, "%d failed\n", failures);
		return 1;
	}
	printf("ok\n");
	return 0;
}
