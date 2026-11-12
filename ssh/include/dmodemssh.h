/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * dmodemssh.h - D-Modem's SSH client, on libssh: the one engine every
 * app has (the desktops', iOS's, and Android's through JNI), so that
 * each has the same SSH.
 *
 * A connection (dmc_ssh) is driven by one thread that calls
 * dmc_ssh_poll() in a loop and acts on the events it hands back: the
 * server's key to trust or not, a login method to choose, a
 * keyboard-interactive round to answer, a channel's data.  Every other
 * call may come from any thread (a lock is taken): what is typed, a
 * window's new size, the answers.
 *
 * What it has: host keys checked by the caller (and in OpenSSH's
 * known_hosts when asked); logins by password, keyboard-interactive, a
 * private key (OpenSSH's, PEM or PuTTY's .ppk), an agent (ssh-agent's
 * socket; on Windows Pageant or OpenSSH's), or OpenSSH's own keys and
 * agent at once (ssh's default); ~/.ssh/config when asked; Ed25519,
 * ECDSA and RSA keys; a shell on a pty, a command, direct TCP channels
 * (ssh -L and -D), remote listens (ssh -R) and the server's
 * agent channels (ssh -A) handed to the caller as channels.
 *
 * This file and dmodemssh.c are the same in D-Modem/gui (ssh/) and
 * D-Modem/app (native-ssh/): change both.
 */
#ifndef DMODEMSSH_H
#define DMODEMSSH_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dmc_ssh dmc_ssh;

/* what dmc_ssh_poll() hands back */
enum {
	DMC_SSH_EV_HOSTKEY = 1, /* the server's key: data is its OpenSSH line
				   ("ssh-ed25519 AAAA..."), value a
				   DMC_SSH_HOST_*; answer dmc_ssh_trust() */
	DMC_SSH_EV_AUTH,	/* a login method wanted: value is the
				   DMC_SSH_AUTH_* the server takes, data why
				   the last failed ("" the first time);
				   answer one dmc_ssh_auth_*() */
	DMC_SSH_EV_PROMPTS,	/* the server's questions (keyboard-
				   interactive): value how many, data the
				   round's name; read them with
				   dmc_ssh_prompt(), answer each with
				   dmc_ssh_answer(), then dmc_ssh_answered() */
	DMC_SSH_EV_READY,	/* logged in: data the server's banner, if any */
	DMC_SSH_EV_OPEN,	/* a channel opened (value 1) or refused
				   (value 0, data why) */
	DMC_SSH_EV_DATA,	/* a channel's data (value 1: its stderr) */
	DMC_SSH_EV_EOF,		/* a channel's end of data */
	DMC_SSH_EV_EXIT,	/* a command's exit status (value), or the
				   signal that ended it (data) */
	DMC_SSH_EV_CLOSE,	/* a channel closed: its number is free */
	DMC_SSH_EV_INCOMING,	/* a channel the server opened, open now:
				   value DMC_SSH_IN_AGENT, or the remote
				   listen's port (data: who connected) */
	DMC_SSH_EV_LISTEN,	/* a remote listen (channel: its request's
				   number) answered: value the port bound,
				   -1 refused */
	DMC_SSH_EV_CLOSED	/* the connection over: data why (NULL when
				   dmc_ssh_disconnect() asked) */
};

typedef struct dmc_ssh_event {
	int type;
	int channel;		/* the channel it's about (1 and up), or 0 */
	int value;
	const char *data;	/* until the next dmc_ssh_poll() */
	int len;
} dmc_ssh_event;

/* the server's key against OpenSSH's known_hosts (when the
   "knownhosts" option was given) */
enum {
	DMC_SSH_HOST_UNCHECKED = 0, DMC_SSH_HOST_KNOWN, DMC_SSH_HOST_NEW,
	DMC_SSH_HOST_CHANGED, DMC_SSH_HOST_OTHER_TYPE
};

/* the login methods the server takes */
enum {
	DMC_SSH_AUTH_PASSWORD = 1, DMC_SSH_AUTH_PUBLICKEY = 2,
	DMC_SSH_AUTH_INTERACTIVE = 4
};

/* a server-opened channel that is the agent's (ssh -A) */
enum { DMC_SSH_IN_AGENT = -1 };

/* a connection to make to user@host:port (nothing done until polled) */
dmc_ssh *dmc_ssh_new(const char *host, int port, const char *user);
/* the connection dropped (if it's up) and freed */
void dmc_ssh_free(dmc_ssh *s);
/* an option, before the first poll: 0, or -1 if it's not one.
     "config"      OpenSSH's config read (a path; "" its own ~/.ssh/config)
     "knownhosts"  the host's key looked up there (a path; "" OpenSSH's)
     "hostkeys"    the host key algorithms, in order (a list with commas)
     "timeout"     seconds to connect in
     "compression" "yes" or "no"
     "agent"       the agent logged in with: a socket (Linux, macOS; ""
                   SSH_AUTH_SOCK's); on Windows "pageant", a named pipe,
                   or "" for Pageant and else OpenSSH's */
int dmc_ssh_option(dmc_ssh *s, const char *name, const char *value);
/* the next event: 1 (in *ev), 0 when timeout_ms passed with none, -1
   when the connection is over and its DMC_SSH_EV_CLOSED was handed back */
int dmc_ssh_poll(dmc_ssh *s, int timeout_ms, dmc_ssh_event *ev);
/* the connection closed by the user: a DMC_SSH_EV_CLOSED with NULL comes */
void dmc_ssh_disconnect(dmc_ssh *s);

/* the server's key: trusted (ok) or not; remember: added to known_hosts */
void dmc_ssh_trust(dmc_ssh *s, int ok, int remember);
/* a login tried (an answer to DMC_SSH_EV_AUTH) */
void dmc_ssh_auth_password(dmc_ssh *s, const char *password);
/* by a private key's text (OpenSSH's, PEM or PuTTY's .ppk): 0, or -1 when it can't be
   read with that passphrase (nothing is tried: answer again) */
int dmc_ssh_auth_key(dmc_ssh *s, const char *key, const char *passphrase);
/* by the agent: the "agent" option's, else the system's (SSH_AUTH_SOCK;
   on Windows Pageant, else OpenSSH's) */
void dmc_ssh_auth_agent(dmc_ssh *s);
/* whether that agent is there to answer (agent as the "agent" option):
   1 or 0 */
int dmc_ssh_agent_reachable(const char *agent);
/* as OpenSSH's ssh does: the agent, then ~/.ssh's keys with no passphrase */
void dmc_ssh_auth_auto(dmc_ssh *s);
/* by the server's questions (keyboard-interactive) */
void dmc_ssh_auth_interactive(dmc_ssh *s);
/* no more tries: the connection closes, "not logged in" */
void dmc_ssh_auth_cancel(dmc_ssh *s);
/* a DMC_SSH_EV_PROMPTS round: its instruction, and question i (*echo:
   whether the answer may be shown) */
const char *dmc_ssh_instruction(dmc_ssh *s);
const char *dmc_ssh_prompt(dmc_ssh *s, int i, int *echo);
void dmc_ssh_answer(dmc_ssh *s, int i, const char *answer);
void dmc_ssh_answered(dmc_ssh *s);

/* channels, once ready: each a number (1 and up), or -1; a
   DMC_SSH_EV_OPEN tells whether it opened */
/* a shell on a pty of term in the given size; agent: ssh -A asked */
int dmc_ssh_shell(dmc_ssh *s, const char *term, int cols, int rows, int agent);
/* a command run, with no pty; agent: ssh -A asked */
int dmc_ssh_exec(dmc_ssh *s, const char *command, int agent);
/* a TCP connection made from the server to host:port (ssh -L, -D), for
   a connection that came from from_host:from_port */
int dmc_ssh_direct(dmc_ssh *s, const char *host, int port,
		   const char *from_host, int from_port);
/* the server asked to listen on address:port (ssh -R; NULL: its
   default, 0: a port it picks): a request's number, answered by a
   DMC_SSH_EV_LISTEN; its connections come as DMC_SSH_EV_INCOMING */
int dmc_ssh_listen(dmc_ssh *s, const char *address, int port);
/* that listen stopped */
void dmc_ssh_unlisten(dmc_ssh *s, const char *address, int port);
/* data to a channel (kept until the server can take it): how many
   bytes are still kept (0 when all went; len 0 asks), or -1 if the
   channel isn't open */
int dmc_ssh_write(dmc_ssh *s, int channel, const char *data, int len);
/* the channel's end of data sent */
void dmc_ssh_eof(dmc_ssh *s, int channel);
/* the channel closed (a DMC_SSH_EV_CLOSE comes) */
void dmc_ssh_close(dmc_ssh *s, int channel);
/* a shell's pty resized */
void dmc_ssh_resize(dmc_ssh *s, int channel, int cols, int rows);

/* keys */
/* OpenSSH's SHA256 fingerprint of a key's OpenSSH line
   ("SHA256:..."), into buf: 0, or -1 if it isn't a key */
int dmc_ssh_fingerprint(const char *keyline, char *buf, int max);
/* a new key ("ed25519", "ecdsa" with bits 256, 384 or 521, "rsa" with
   bits): its private text (OpenSSH's, encrypted with passphrase unless
   it's NULL or "") and its public line, both to free with
   dmc_ssh_free_string(): 0, or -1 */
int dmc_ssh_keygen(const char *type, int bits, const char *passphrase,
		   char **private_key, char **public_key);
/* a private key's public line: 0, or -1 if it can't be read with that
   passphrase */
int dmc_ssh_key_public(const char *key, const char *passphrase,
		       char **public_key);
/* whether a private key's text (OpenSSH's, PEM or PuTTY's .ppk) needs a
   passphrase: 1 or 0 */
int dmc_ssh_key_encrypted(const char *key);
/* a private key written again (OpenSSH's form) with a new passphrase
   (NULL or "": none): 0, or -1 */
int dmc_ssh_key_export(const char *key, const char *passphrase,
		       const char *new_passphrase, char **out);
void dmc_ssh_free_string(char *str);

#ifdef __cplusplus
}
#endif

#endif
