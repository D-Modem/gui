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
		CHECK(dmc_ssh_key_export(priv, NULL, "sesame", &enc) == 0);
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

/* the logins and channels against DMODEM_SSH_TEST's sshd */
static void test_live(const char *spec)
{
	static char buf[512];
	char *p[4];
	int i;
	dmc_ssh *s;
	dmc_ssh_event ev;
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
}

int main(void)
{
	const char *spec = getenv("DMODEM_SSH_TEST");

#ifdef _WIN32
	WSADATA w;
	WSAStartup(MAKEWORD(2, 2), &w);
#endif
	test_keys();
	if (spec)
		test_live(spec);
	else
		printf("DMODEM_SSH_TEST is not set: no login tested\n");
	if (failures) {
		fprintf(stderr, "%d failed\n", failures);
		return 1;
	}
	printf("ok\n");
	return 0;
}
