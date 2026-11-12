/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * remote.c - the Remote page: a login to a server by SSH, Telnet or
 *            rlogin, in a terminal of its own, beside the modem's
 *
 * Telnet and rlogin are a TCP connection made here and libdmodemctl's
 * dmc_net for the protocol.  SSH is libdmodemssh (the apps' own client,
 * on libssh) on a thread of its own, its events brought to the main
 * loop; it reads OpenSSH's config, known_hosts, keys and agent, so that
 * they are the user's.  Its login may instead be by the agent alone (an
 * ssh-agent's socket, SSH_AUTH_SOCK's or one given) or by a key file:
 * OpenSSH's, PEM or PuTTY's .ppk, which can also be imported into
 * ~/.ssh in OpenSSH's form.  Either way the terminal only shows bytes
 * and gives what is typed.
 */
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "app.h"
#include "dmodemssh.h"

/* the protocols in the drop-down's order */
static const int protos[] = { DMC_NET_SSH, DMC_NET_TELNET, DMC_NET_RLOGIN };

typedef struct Remote {
	App *a;
	GtkWidget *proto, *host, *port, *user, *go, *status;
	/* how SSH logs in: its menu, the way, the key file, the agent */
	GtkWidget *login, *auth, *key_label, *agent, *agent_state;
	GtkWidget *term, *type;
	int cols, rows;
	/* what runs: its protocol (0: none) */
	int running;
	/* Telnet, rlogin: the socket and the protocol */
	GSocketClient *client;
	GCancellable *cancel;
	GSocketConnection *conn;
	GSocket *sock;
	GSource *in_src, *out_src;
	dmc_net *net;
	GByteArray *pending;
	/* SSH: the connection (its thread polls it), the shell's channel,
	   the logins tried, a question's dialog */
	struct Ssh *ssh;
	int shell, tried, asks, exit_status;
	/* the methods the server last offered; the key file's text and the
	   passphrases tried for it */
	int methods, key_tries;
	char *key_text;
	AdwDialog *dialog;
	guint size_tick;
} Remote;

static Remote *R;

static void set_status(Remote *r, const char *text)
{
	gtk_label_set_text(GTK_LABEL(r->status), text);
}

static void controls(Remote *r)
{
	gboolean idle = !r->running;
	gtk_widget_set_sensitive(r->proto, idle);
	gtk_widget_set_sensitive(r->host, idle);
	gtk_widget_set_sensitive(r->port, idle);
	gtk_widget_set_sensitive(r->user, idle);
	if (r->login)
		gtk_widget_set_sensitive(r->login, idle);
	gtk_button_set_label(GTK_BUTTON(r->go), idle ? "Connect" : "Disconnect");
	if (idle) {
		gtk_widget_add_css_class(r->go, "suggested-action");
		gtk_widget_remove_css_class(r->go, "destructive-action");
	} else {
		gtk_widget_remove_css_class(r->go, "suggested-action");
		gtk_widget_add_css_class(r->go, "destructive-action");
	}
}

static void show(Remote *r, const char *data, gssize n)
{
	termview_feed(r->term, data, n < 0 ? (int)strlen(data) : (int)n);
}

/* the key file's text let go of (wiped) */
static void key_forget(Remote *r)
{
	if (r->key_text) {
		memset(r->key_text, 0, strlen(r->key_text));
		g_clear_pointer(&r->key_text, g_free);
	}
}

static void ssh_disconnect(Remote *r);

/* the connection over, with why (NULL: the user's doing) */
static void ended(Remote *r, const char *why)
{
	if (!r->running)
		return;
	r->running = 0;
	if (r->cancel) {
		g_cancellable_cancel(r->cancel);
		g_clear_object(&r->cancel);
	}
	if (r->in_src) {
		g_source_destroy(r->in_src);
		g_clear_pointer(&r->in_src, g_source_unref);
	}
	if (r->out_src) {
		g_source_destroy(r->out_src);
		g_clear_pointer(&r->out_src, g_source_unref);
	}
	if (r->conn)
		g_io_stream_close(G_IO_STREAM(r->conn), NULL, NULL);
	g_clear_object(&r->conn);
	r->sock = NULL;
	g_clear_object(&r->client);
	g_clear_pointer(&r->net, dmc_net_free);
	if (r->pending)
		g_byte_array_set_size(r->pending, 0);
	/* (the connection is freed once its thread has said it's closed) */
	if (r->ssh)
		ssh_disconnect(r);
	key_forget(r);
	r->shell = 0;
	if (r->size_tick)
		g_source_remove(r->size_tick);
	r->size_tick = 0;
	char *line = g_strdup_printf("\r\n\x1b[0m[%s]\r\n", why ? why :
				     "Disconnected");
	show(r, line, -1);
	g_free(line);
	set_status(r, why ? why : "Disconnected");
	controls(r);
}

/*
 *    Telnet and rlogin
 */

static gboolean writable(GSocket *s, GIOCondition c, gpointer data);

/* the protocol's output to the socket, as much as it takes now */
static void flush(Remote *r)
{
	char buf[4096];
	int n;
	while ((n = dmc_net_output(r->net, buf, sizeof(buf))) > 0)
		g_byte_array_append(r->pending, (guint8 *)buf, n);
	while (r->pending->len > 0) {
		GError *e = NULL;
		gssize w = g_socket_send(r->sock, (char *)r->pending->data,
					 r->pending->len, NULL, &e);
		if (w < 0) {
			if (g_error_matches(e, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
				g_error_free(e);
				if (!r->out_src) {
					r->out_src = g_socket_create_source(
						r->sock, G_IO_OUT, NULL);
					g_source_set_callback(r->out_src,
						(GSourceFunc)(void (*)(void))writable,
						r, NULL);
					g_source_attach(r->out_src, NULL);
				}
				return;
			}
			char *why = g_strdup(e->message);
			g_error_free(e);
			ended(r, why);
			g_free(why);
			return;
		}
		g_byte_array_remove_range(r->pending, 0, (guint)w);
	}
}

static gboolean writable(GSocket *s, GIOCondition c, gpointer data)
{
	Remote *r = data;
	(void)s;
	(void)c;
	g_clear_pointer(&r->out_src, g_source_unref);
	flush(r);
	return G_SOURCE_REMOVE;
}

static void pump(Remote *r)
{
	char buf[8192];
	int n;
	while ((n = dmc_net_screen(r->net, buf, sizeof(buf))) > 0)
		show(r, buf, n);
	flush(r);
}

static gboolean readable(GSocket *s, GIOCondition c, gpointer data)
{
	Remote *r = data;
	char buf[8192];
	/* rlogin's control byte, sent urgent */
	if (c & G_IO_PRI) {
		char b;
		if (recv(g_socket_get_fd(s), &b, 1, MSG_OOB) == 1)
			dmc_net_urgent(r->net, (unsigned char)b);
	}
	if (c & (G_IO_IN | G_IO_HUP | G_IO_ERR)) {
		GError *e = NULL;
		gssize n = g_socket_receive(s, buf, sizeof(buf), NULL, &e);
		if (n < 0 && g_error_matches(e, G_IO_ERROR,
					     G_IO_ERROR_WOULD_BLOCK)) {
			g_error_free(e);
		} else if (n <= 0) {
			char *why = e ? g_strdup(e->message) :
				g_strdup("Connection closed by the server");
			g_clear_error(&e);
			pump(r);
			ended(r, why);
			g_free(why);
			return G_SOURCE_REMOVE;
		} else {
			dmc_net_feed(r->net, buf, (int)n);
		}
	}
	pump(r);
	return G_SOURCE_CONTINUE;
}

static void connected(GObject *src, GAsyncResult *res, gpointer data)
{
	Remote *r = data;
	GError *e = NULL;
	GSocketConnection *c = g_socket_client_connect_to_host_finish(
		G_SOCKET_CLIENT(src), res, &e);
	if (!c) {
		/* (cancelled: ended() has said so) */
		if (!g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			ended(r, e->message);
		g_error_free(e);
		return;
	}
	r->conn = c;
	r->sock = g_socket_connection_get_socket(c);
	g_socket_set_blocking(r->sock, FALSE);
	g_socket_set_option(r->sock, IPPROTO_TCP, TCP_NODELAY, 1, NULL);
	r->in_src = g_socket_create_source(r->sock, G_IO_IN | G_IO_PRI |
					   G_IO_HUP | G_IO_ERR, NULL);
	g_source_set_callback(r->in_src, (GSourceFunc)(void (*)(void))readable,
			      r, NULL);
	g_source_attach(r->in_src, NULL);
	set_status(r, "Connected");
	dmc_net_connected(r->net);
	pump(r);
}

/*
 *    SSH: libdmodemssh, polled on a thread of its own
 */

/* a connection and the thread that polls it */
typedef struct Ssh {
	dmc_ssh *s;
	GThread *thread;
} Ssh;

/* one of its events, brought to the main loop */
typedef struct SshEv {
	Remote *r;
	Ssh *ssh;
	int type, channel, value, len;
	char *data;
} SshEv;

static void ssh_ev_free(gpointer p)
{
	SshEv *e = p;
	g_free(e->data);
	g_free(e);
}

static gboolean ssh_event(gpointer p);

/* the connection polled until it's over, each event to the main loop */
static gpointer ssh_poll(gpointer p)
{
	SshEv *start = p;
	Remote *r = start->r;
	Ssh *ssh = start->ssh;
	dmc_ssh_event ev;
	int n;

	g_free(start);
	while ((n = dmc_ssh_poll(ssh->s, 1000, &ev)) >= 0) {
		if (n == 0)
			continue;
		SshEv *e = g_new0(SshEv, 1);
		e->r = r;
		e->ssh = ssh;
		e->type = ev.type;
		e->channel = ev.channel;
		e->value = ev.value;
		e->len = ev.len;
		e->data = ev.data ? g_memdup2(ev.data, (gsize)ev.len + 1) : NULL;
		g_main_context_invoke_full(NULL, G_PRIORITY_DEFAULT, ssh_event,
					   e, ssh_ev_free);
		if (ev.type == DMC_SSH_EV_CLOSED)
			break;
	}
	return NULL;
}

/* a dialog of the login's closed: its answer, or none (cancel) */
static void dialog_gone(Remote *r)
{
	if (r->dialog) {
		AdwDialog *d = r->dialog;
		r->dialog = NULL;
		adw_dialog_force_close(d);
	}
}

/* the host key dialog answered */
static void hostkey_answered(AdwAlertDialog *d, const char *response, gpointer data)
{
	Remote *r = data;
	Ssh *ssh = g_object_get_data(G_OBJECT(d), "ssh");
	(void)d;
	r->dialog = NULL;
	if (ssh != r->ssh)
		return;
	dmc_ssh_trust(ssh->s, !strcmp(response, "connect"), 1);
}

/* the server's key: known goes on; new is asked; changed is refused */
static void ssh_hostkey(Remote *r, const char *key, int known)
{
	char fp[128] = "";
	dmc_ssh_fingerprint(key, fp, sizeof(fp));
	if (known == DMC_SSH_HOST_KNOWN) {
		dmc_ssh_trust(r->ssh->s, 1, 0);
		return;
	}
	if (known == DMC_SSH_HOST_CHANGED || known == DMC_SSH_HOST_OTHER_TYPE) {
		char *m = g_strdup_printf(
			"\r\n\x1b[1;31mThe server's host key has changed (it is now %.*s %s).\r\n"
			"Someone may be in the middle; or the server has a new key. If that is\r\n"
			"expected, remove its old line from ~/.ssh/known_hosts.\x1b[0m\r\n",
			(int)strcspn(key, " "), key, fp);
		show(r, m, -1);
		g_free(m);
		dmc_ssh_trust(r->ssh->s, 0, 0);
		return;
	}
	char *body = g_strdup_printf(
		"This server is not known yet. Its key's fingerprint is\n\n%.*s %s\n\n"
		"If it's the one you expect, it will be added to ~/.ssh/known_hosts.",
		(int)strcspn(key, " "), key, fp);
	AdwDialog *d = adw_alert_dialog_new("Connect to a new server?", body);
	g_free(body);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(d), "cancel", "Cancel",
				       "connect", "Connect", NULL);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(d), "connect",
						 ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(d), "connect");
	adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(d), "cancel");
	g_object_set_data(G_OBJECT(d), "ssh", r->ssh);
	g_signal_connect(d, "response", G_CALLBACK(hostkey_answered), r);
	r->dialog = d;
	adw_dialog_present(d, GTK_WIDGET(r->a->window));
}

/* a password or the server's questions answered (or cancelled) */
static void answers_given(AdwAlertDialog *d, const char *response, gpointer data)
{
	Remote *r = data;
	Ssh *ssh = g_object_get_data(G_OBJECT(d), "ssh");
	GPtrArray *entries = g_object_get_data(G_OBJECT(d), "entries");
	int prompts = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(d), "prompts"));
	r->dialog = NULL;
	if (ssh != r->ssh)
		return;
	if (strcmp(response, "ok")) {
		dmc_ssh_auth_cancel(ssh->s);
		return;
	}
	if (!prompts) {
		dmc_ssh_auth_password(ssh->s, gtk_editable_get_text(
			GTK_EDITABLE(g_ptr_array_index(entries, 0))));
		return;
	}
	for (guint i = 0; i < entries->len; i++)
		dmc_ssh_answer(ssh->s, (int)i, gtk_editable_get_text(
			GTK_EDITABLE(g_ptr_array_index(entries, i))));
	dmc_ssh_answered(ssh->s);
}

/* a dialog asking for a password (prompts 0) or the server's questions */
static void ask(Remote *r, const char *heading, const char *body, int prompts)
{
	AdwDialog *d = adw_alert_dialog_new(heading, body && *body ? body : NULL);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	GPtrArray *entries = g_ptr_array_new();
	int n = prompts ? prompts : 1;
	for (int i = 0; i < n; i++) {
		int echo = 0;
		const char *q = prompts ? dmc_ssh_prompt(r->ssh->s, i, &echo) : "Password";
		GtkWidget *e = echo ? gtk_entry_new() : gtk_password_entry_new();
		if (echo)
			gtk_entry_set_placeholder_text(GTK_ENTRY(e), q);
		else
			g_object_set(e, "placeholder-text", q, NULL);
		if (prompts) {
			GtkWidget *l = gtk_label_new(q);
			gtk_label_set_xalign(GTK_LABEL(l), 0);
			gtk_label_set_wrap(GTK_LABEL(l), TRUE);
			gtk_box_append(GTK_BOX(box), l);
		}
		gtk_box_append(GTK_BOX(box), e);
		g_ptr_array_add(entries, e);
		/* (Enter answers) */
		g_object_set(e, "activates-default", TRUE, NULL);
	}
	adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(d), box);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(d), "cancel", "Cancel",
				       "ok", "Log In", NULL);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(d), "ok",
						 ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(d), "ok");
	adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(d), "cancel");
	g_object_set_data(G_OBJECT(d), "ssh", r->ssh);
	g_object_set_data_full(G_OBJECT(d), "entries", entries,
			       (GDestroyNotify)g_ptr_array_unref);
	g_object_set_data(G_OBJECT(d), "prompts", GINT_TO_POINTER(prompts));
	g_signal_connect(d, "response", G_CALLBACK(answers_given), r);
	r->dialog = d;
	adw_dialog_present(d, GTK_WIDGET(r->a->window));
	gtk_widget_grab_focus(g_ptr_array_index(entries, 0));
}

/* a passphrase given (NULL: cancelled), to what asked for it */
typedef void (*PassFn)(Remote *r, const char *pass, gpointer data);

static void passphrase_given(AdwAlertDialog *d, const char *response, gpointer data)
{
	Remote *r = data;
	PassFn fn = g_object_get_data(G_OBJECT(d), "fn");
	GtkWidget *e = g_object_get_data(G_OBJECT(d), "entry");
	if (r->dialog == ADW_DIALOG(d))
		r->dialog = NULL;
	fn(r, strcmp(response, "ok") ? NULL : gtk_editable_get_text(GTK_EDITABLE(e)),
	   g_object_get_data(G_OBJECT(d), "arg"));
}

/* a key's passphrase asked for (why: a try's failure, or NULL); fn
   gets it with arg (freed with free_arg); login: the connection's
   dialog, closed with it */
static void ask_passphrase(Remote *r, const char *name, const char *why,
			   PassFn fn, gpointer arg, GDestroyNotify free_arg,
			   gboolean login)
{
	char *head = g_strdup_printf("Passphrase for %s", name);
	AdwDialog *d = adw_alert_dialog_new(head, why);
	g_free(head);
	GtkWidget *e = gtk_password_entry_new();
	g_object_set(e, "placeholder-text", "Passphrase", "activates-default", TRUE, NULL);
	adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(d), e);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(d), "cancel", "Cancel",
				       "ok", "Unlock", NULL);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(d), "ok",
						 ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(d), "ok");
	adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(d), "cancel");
	g_object_set_data(G_OBJECT(d), "fn", (gpointer)fn);
	g_object_set_data(G_OBJECT(d), "entry", e);
	g_object_set_data_full(G_OBJECT(d), "arg", arg, free_arg);
	g_signal_connect(d, "response", G_CALLBACK(passphrase_given), r);
	if (login)
		r->dialog = d;
	adw_dialog_present(d, GTK_WIDGET(r->a->window));
	gtk_widget_grab_focus(e);
}

/* a path with ~/ at its start made the home folder's */
static char *expand(const char *path)
{
	if (path[0] == '~' && path[1] == '/')
		return g_build_filename(g_get_home_dir(), path + 2, NULL);
	return g_strdup(path);
}

/* what's tried, in OpenSSH's order: keys and the agent (or what was
   chosen: the agent alone, or a key file), then the server's questions
   or a password (three times at most) */
enum { TRIED_KEYS = 1, TRIED_FILE = 2 };

static void ssh_auth(Remote *r, int methods, const char *why);

/* the key file's passphrase given: the login by it, or another try */
static void key_passphrase(Remote *r, const char *pass, gpointer data)
{
	Ssh *ssh = data;
	if (ssh != r->ssh || !r->key_text)
		return;
	char *name = g_path_get_basename(r->a->cfg.remote_key);
	if (pass && dmc_ssh_auth_key(ssh->s, r->key_text, pass) == 0) {
		r->tried |= TRIED_FILE;
	} else if (pass && ++r->key_tries < 3) {
		ask_passphrase(r, name, "That passphrase does not open the key.",
			       key_passphrase, ssh, NULL, TRUE);
	} else {
		r->tried |= TRIED_FILE;
		key_forget(r);
		ssh_auth(r, r->methods, "");
	}
	g_free(name);
}

/* the login by the key file: its passphrase asked for if it has one;
   FALSE when there's nothing to try */
static gboolean key_login(Remote *r)
{
	char *path = expand(r->a->cfg.remote_key);
	char *name = g_path_get_basename(path);
	gboolean ok = FALSE;

	key_forget(r);
	if (!g_file_get_contents(path, &r->key_text, NULL, NULL)) {
		char *m = g_strdup_printf("\r\n[The key file %s can't be read]\r\n", path);
		show(r, m, -1);
		g_free(m);
		r->tried |= TRIED_FILE;
	} else if (dmc_ssh_key_encrypted(r->key_text)) {
		r->key_tries = 0;
		ask_passphrase(r, name, NULL, key_passphrase, r->ssh, NULL, TRUE);
		ok = TRUE;
	} else {
		r->tried |= TRIED_FILE;
		ok = dmc_ssh_auth_key(r->ssh->s, r->key_text, NULL) == 0;
		if (!ok) {
			char *m = g_strdup_printf("\r\n[%s is not a key that can be read]\r\n", path);
			show(r, m, -1);
			g_free(m);
		}
	}
	g_free(name);
	g_free(path);
	return ok;
}

static void ssh_auth(Remote *r, int methods, const char *why)
{
	Ssh *ssh = r->ssh;
	const char *how = r->a->cfg.remote_auth;
	gboolean file = !strcmp(how, "key") && *r->a->cfg.remote_key;

	r->methods = methods;
	if ((methods & DMC_SSH_AUTH_PUBLICKEY) && file && !(r->tried & TRIED_FILE)) {
		if (key_login(r))
			return;
	} else if ((methods & DMC_SSH_AUTH_PUBLICKEY) && !file &&
		   !(r->tried & TRIED_KEYS)) {
		r->tried |= TRIED_KEYS;
		if (!strcmp(how, "agent"))
			dmc_ssh_auth_agent(ssh->s);
		else
			dmc_ssh_auth_auto(ssh->s);
		return;
	}
	if (r->asks++ >= 3) {
		dmc_ssh_auth_cancel(ssh->s);
		return;
	}
	const char *body = r->asks > 1 && *why ? why : NULL;
	char *who = g_strdup_printf("Log in to %s", gtk_editable_get_text(
		GTK_EDITABLE(r->host)));
	if (methods & DMC_SSH_AUTH_INTERACTIVE)
		dmc_ssh_auth_interactive(ssh->s);
	else if (methods & DMC_SSH_AUTH_PASSWORD)
		ask(r, who, body, 0);
	else
		dmc_ssh_auth_cancel(ssh->s);
	g_free(who);
}

/* an event of the connection's, on the main loop */
static gboolean ssh_event(gpointer p)
{
	SshEv *e = p;
	Remote *r = e->r;
	Ssh *ssh = e->ssh;

	if (e->type == DMC_SSH_EV_CLOSED) {
		/* (its thread has ended: the connection can go) */
		g_thread_join(ssh->thread);
		dmc_ssh_free(ssh->s);
		g_free(ssh);
		if (ssh != r->ssh)
			return G_SOURCE_REMOVE;
		r->ssh = NULL;
		dialog_gone(r);
		if (!e->data && r->exit_status > 0) {
			char *why = g_strdup_printf("the shell exited with status %d",
						    r->exit_status);
			ended(r, why);
			g_free(why);
		} else {
			ended(r, e->data);
		}
		return G_SOURCE_REMOVE;
	}
	if (ssh != r->ssh)
		return G_SOURCE_REMOVE;
	switch (e->type) {
	case DMC_SSH_EV_HOSTKEY:
		ssh_hostkey(r, e->data, e->value);
		break;
	case DMC_SSH_EV_AUTH:
		ssh_auth(r, e->value, e->data);
		break;
	case DMC_SSH_EV_PROMPTS:
		if (e->value == 0) {
			dmc_ssh_answered(ssh->s);
		} else {
			ask(r, e->data && *e->data ? e->data : "The server asks",
			    dmc_ssh_instruction(ssh->s), e->value);
		}
		break;
	case DMC_SSH_EV_READY:
		if (e->data && *e->data) {
			show(r, e->data, e->len);
		}
		r->shell = dmc_ssh_shell(ssh->s, dmc_vt_profile_term(termview_profile(r->term)), r->cols, r->rows, 0);
		set_status(r, "Connected by SSH");
		break;
	case DMC_SSH_EV_OPEN:
		if (!e->value) {
			show(r, "\r\n[The server refused the shell]\r\n", -1);
			dmc_ssh_disconnect(ssh->s);
		}
		break;
	case DMC_SSH_EV_DATA:
		if (e->channel == r->shell)
			show(r, e->data, e->len);
		break;
	case DMC_SSH_EV_EXIT:
		if (e->channel == r->shell)
			r->exit_status = e->value;
		break;
	case DMC_SSH_EV_CLOSE:
		/* (the shell's over: so is the connection) */
		if (e->channel == r->shell)
			dmc_ssh_disconnect(ssh->s);
		break;
	case DMC_SSH_EV_INCOMING:
		/* (nothing was asked for that opens these) */
		dmc_ssh_close(ssh->s, e->channel);
		break;
	}
	return G_SOURCE_REMOVE;
}

/* the connection closed by the user: its thread ends with it */
static void ssh_disconnect(Remote *r)
{
	dialog_gone(r);
	dmc_ssh_disconnect(r->ssh->s);
}

static gboolean start_ssh(Remote *r, const char *host, int port,
			  const char *user)
{
	dmc_ssh *s = dmc_ssh_new(host, port, *user ? user : g_get_user_name());
	if (!s) {
		window_toast(r->a, "Could not start SSH");
		return FALSE;
	}
	/* OpenSSH's own config and known hosts: the user's */
	dmc_ssh_option(s, "config", "");
	dmc_ssh_option(s, "knownhosts", "");
	/* the agent: the one given, else SSH_AUTH_SOCK's */
	const char *agent = r->a->cfg.remote_agent;
	if (*agent)
		dmc_ssh_option(s, "agent", agent);
	if (!strcmp(r->a->cfg.remote_auth, "agent") && !dmc_ssh_agent_reachable(agent))
		show(r, *agent ? "[No agent answers at the socket given]\r\n" :
		     "[No agent answers: SSH_AUTH_SOCK names none]\r\n", -1);
	Ssh *ssh = g_new0(Ssh, 1);
	ssh->s = s;
	r->ssh = ssh;
	r->shell = 0;
	r->tried = 0;
	r->asks = 0;
	key_forget(r);
	r->exit_status = 0;
	SshEv *start = g_new0(SshEv, 1);
	start->r = r;
	start->ssh = ssh;
	ssh->thread = g_thread_new("ssh", ssh_poll, start);
	return TRUE;
}

/*
 *    the page
 */

/* the terminal's size, told to the far end as it changes */
static gboolean size_check(gpointer data)
{
	Remote *r = data;
	int c, w;
	termview_size(r->term, &c, &w);
	if (c == r->cols && w == r->rows)
		return G_SOURCE_CONTINUE;
	r->cols = c;
	r->rows = w;
	if (r->net && r->sock) {
		dmc_net_resize(r->net, c, w);
		flush(r);
	} else if (r->ssh && r->shell > 0) {
		dmc_ssh_resize(r->ssh->s, r->shell, c, w);
	}
	return G_SOURCE_CONTINUE;
}

static void committed(const char *text, int size, void *data)
{
	Remote *r = data;
	if (!r->running)
		return;
	if (r->ssh && r->shell > 0) {
		dmc_ssh_write(r->ssh->s, r->shell, text, (int)size);
	} else if (r->net && r->sock) {
		dmc_net_type(r->net, text, (int)size);
		flush(r);
	}
}

static int chosen_proto(Remote *r)
{
	guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(r->proto));
	return i < G_N_ELEMENTS(protos) ? protos[i] : DMC_NET_SSH;
}

/* the default port shown for the protocol */
static void proto_changed(GObject *o, GParamSpec *p, gpointer data)
{
	Remote *r = data;
	(void)o;
	(void)p;
	char ps[16];
	g_snprintf(ps, sizeof(ps), "%d", dmc_net_default_port(chosen_proto(r)));
	gtk_entry_set_placeholder_text(GTK_ENTRY(r->port), ps);
	if (r->login)
		gtk_widget_set_visible(r->login, chosen_proto(r) == DMC_NET_SSH);
}

static void connect_clicked(GtkWidget *w, gpointer data)
{
	Remote *r = data;
	App *a = r->a;
	(void)w;
	if (r->running) {
		ended(r, NULL);
		return;
	}
	char *host = g_strstrip(g_strdup(
		gtk_editable_get_text(GTK_EDITABLE(r->host))));
	const char *user = gtk_editable_get_text(GTK_EDITABLE(r->user));
	const char *pt = gtk_editable_get_text(GTK_EDITABLE(r->port));
	int proto = chosen_proto(r);
	int port = *pt ? atoi(pt) : dmc_net_default_port(proto);
	if (!*host) {
		window_toast(a, "Enter the server to connect to");
		g_free(host);
		return;
	}
	if (port <= 0 || port > 65535) {
		window_toast(a, "The port is a number from 1 to 65535");
		g_free(host);
		return;
	}
	if (proto == DMC_NET_RLOGIN && !*user) {
		window_toast(a, "rlogin needs a user name");
		g_free(host);
		return;
	}
	/* kept for next time */
	g_free(a->cfg.remote_proto);
	a->cfg.remote_proto = g_strdup(dmc_net_name(proto));
	g_free(a->cfg.remote_host);
	a->cfg.remote_host = g_strdup(host);
	a->cfg.remote_port = *pt ? port : 0;
	g_free(a->cfg.remote_user);
	a->cfg.remote_user = g_strdup(user);
	config_save(&a->cfg);

	termview_reset(r->term);
	termview_size(r->term, &r->cols, &r->rows);
	char *what = g_strdup_printf("Connecting to %s port %d by %s", host,
				     port, dmc_net_name(proto));
	set_status(r, what);
	char *line = g_strdup_printf("[%s]\r\n", what);
	show(r, line, -1);
	g_free(line);
	g_free(what);

	if (proto == DMC_NET_SSH) {
		if (!start_ssh(r, host, port, user)) {
			set_status(r, "Not connected");
			g_free(host);
			return;
		}
	} else {
		/* (Telnet's TERMINAL-TYPE, rlogin's TERM) */
		const char *type = termview_profile(r->term);
		r->net = dmc_net_new(proto, user, proto == DMC_NET_TELNET ?
				     dmc_vt_profile_telnet(type) :
				     dmc_vt_profile_term(type), r->cols, r->rows);
		r->client = g_socket_client_new();
		g_socket_client_set_timeout(r->client, 20);
		r->cancel = g_cancellable_new();
		g_socket_client_connect_to_host_async(r->client, host,
						      (guint16)port, r->cancel,
						      connected, r);
	}
	r->running = proto;
	r->size_tick = g_timeout_add(250, size_check, r);
	controls(r);
	termview_focus(r->term);
	g_free(host);
}

static void type_changed(GObject *o, GParamSpec *p, gpointer data)
{
	Remote *r = data;
	(void)o;
	(void)p;
	const char *id = term_types_chosen(r->type);
	termview_set_profile(r->term, id);
	g_free(r->a->cfg.remote_term);
	r->a->cfg.remote_term = g_strdup(id);
	config_save(&r->a->cfg);
}

/*
 *    how SSH logs in: its menu
 */

/* the ways in the drop-down's order, as the config has them */
static const char *const hows[] = { "auto", "agent", "key" };

/* the key file shown (and its row's tooltip) */
static void key_shown(Remote *r)
{
	const char *k = r->a->cfg.remote_key;
	char *name = *k ? g_path_get_basename(k) : g_strdup("No key file chosen");
	gtk_label_set_text(GTK_LABEL(r->key_label), name);
	gtk_widget_set_tooltip_text(r->key_label, *k ? k : NULL);
	g_free(name);
}

/* whether the agent answers, under its socket's entry */
static void agent_shown(Remote *r)
{
	const char *a = r->a->cfg.remote_agent;
	const char *m = dmc_ssh_agent_reachable(a) ? "An agent answers there" :
			*a ? "No agent answers there" :
			g_getenv("SSH_AUTH_SOCK") ? "No agent answers at SSH_AUTH_SOCK" :
			"SSH_AUTH_SOCK is not set: give the agent's socket";
	gtk_label_set_text(GTK_LABEL(r->agent_state), m);
}

/* the way chosen, kept */
static void how_changed(GObject *o, GParamSpec *p, gpointer data)
{
	Remote *r = data;
	(void)o;
	(void)p;
	guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(r->auth));
	g_free(r->a->cfg.remote_auth);
	r->a->cfg.remote_auth = g_strdup(hows[i < G_N_ELEMENTS(hows) ? i : 0]);
	config_save(&r->a->cfg);
}

/* the key file set, and the login by it chosen */
static void key_set(Remote *r, const char *path)
{
	g_free(r->a->cfg.remote_key);
	r->a->cfg.remote_key = g_strdup(path);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(r->auth), 2);
	config_save(&r->a->cfg);
	key_shown(r);
}

/* the agent's socket typed, kept */
static void agent_changed(GtkEditable *e, gpointer data)
{
	Remote *r = data;
	g_free(r->a->cfg.remote_agent);
	r->a->cfg.remote_agent = g_strstrip(g_strdup(gtk_editable_get_text(e)));
	config_save(&r->a->cfg);
	agent_shown(r);
}

/* a file dialog for keys, in ~/.ssh, *.ppk alone when ppk */
static GtkFileDialog *key_dialog(const char *title, gboolean ppk)
{
	GtkFileDialog *d = gtk_file_dialog_new();
	gtk_file_dialog_set_title(d, title);
	char *dir = g_build_filename(g_get_home_dir(), ".ssh", NULL);
	GFile *f = g_file_new_for_path(dir);
	if (g_file_test(dir, G_FILE_TEST_IS_DIR))
		gtk_file_dialog_set_initial_folder(d, f);
	g_object_unref(f);
	g_free(dir);
	GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
	GtkFileFilter *putty = gtk_file_filter_new();
	gtk_file_filter_set_name(putty, "PuTTY keys (.ppk)");
	gtk_file_filter_add_pattern(putty, "*.ppk");
	if (!ppk) {
		GtkFileFilter *all = gtk_file_filter_new();
		gtk_file_filter_set_name(all, "All keys (OpenSSH's, PEM, PuTTY's)");
		gtk_file_filter_add_pattern(all, "*");
		g_list_store_append(filters, all);
		g_object_unref(all);
	}
	g_list_store_append(filters, putty);
	g_object_unref(putty);
	gtk_file_dialog_set_filters(d, G_LIST_MODEL(filters));
	g_object_unref(filters);
	return d;
}

static void key_chosen(GObject *src, GAsyncResult *res, gpointer data)
{
	GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
	if (!f)
		return;
	char *p = g_file_get_path(f);
	if (p)
		key_set(data, p);
	g_free(p);
	g_object_unref(f);
}

static void choose_key(GtkButton *b, gpointer data)
{
	Remote *r = data;
	gtk_menu_button_popdown(GTK_MENU_BUTTON(r->login));
	(void)b;
	GtkFileDialog *d = key_dialog("Choose a Private Key", FALSE);
	gtk_file_dialog_open(d, r->a->window, NULL, key_chosen, r);
	g_object_unref(d);
}

/* where an imported key goes: ~/.ssh/<its name>, a name not yet taken */
static char *import_path(const char *ppk)
{
	char *dir = g_build_filename(g_get_home_dir(), ".ssh", NULL);
	char *base = g_path_get_basename(ppk);
	char *dot = strrchr(base, '.');
	if (dot && !g_ascii_strcasecmp(dot, ".ppk"))
		*dot = 0;
	if (!*base) {
		g_free(base);
		base = g_strdup("putty-key");
	}
	g_mkdir_with_parents(dir, 0700);
	char *path = NULL;
	for (int i = 1; i < 100; i++) {
		char *name = i == 1 ? g_strdup(base) : g_strdup_printf("%s-%d", base, i);
		path = g_build_filename(dir, name, NULL);
		char *pub = g_strconcat(path, ".pub", NULL);
		gboolean taken = g_file_test(path, G_FILE_TEST_EXISTS) ||
				 g_file_test(pub, G_FILE_TEST_EXISTS);
		g_free(pub);
		g_free(name);
		if (!taken)
			break;
		g_clear_pointer(&path, g_free);
	}
	g_free(base);
	g_free(dir);
	return path;
}

/* a .ppk imported with its passphrase (NULL: cancelled): written to
   ~/.ssh in OpenSSH's form, with the same passphrase, and chosen */
static void import_with(Remote *r, const char *pass, gpointer data)
{
	const char *ppk = data;
	char *text = NULL, *key = NULL, *pub = NULL;
	char *name = g_path_get_basename(ppk);

	if (!g_file_get_contents(ppk, &text, NULL, NULL)) {
		window_toast(r->a, "The PuTTY key can't be read");
		g_free(name);
		return;
	}
	gboolean enc = dmc_ssh_key_encrypted(text);
	if (enc && !pass)
		goto done;
	if (dmc_ssh_key_export(text, pass, pass, &key) || dmc_ssh_key_public(key, pass, &pub)) {
		if (enc)
			ask_passphrase(r, name, "That passphrase does not open the key.",
				       import_with, g_strdup(ppk), g_free, FALSE);
		else
			window_toast(r->a, "That file is not a PuTTY key that can be read");
		goto done;
	}
	char *path = import_path(ppk);
	char *pubpath = path ? g_strconcat(path, ".pub", NULL) : NULL;
	/* (the .pub line with the .ppk's comment, as OpenSSH's have theirs) */
	const char *c = strstr(text, "\nComment: ");
	char *comment = c ? g_strndup(c + 10, strcspn(c + 10, "\r\n")) : g_strdup("");
	char *line = g_strdup_printf("%s%s%s\n", pub, *comment ? " " : "", comment);
	GError *e = NULL;
	if (path && g_file_set_contents_full(path, key, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, &e) &&
	    g_file_set_contents_full(pubpath, line, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0644, &e)) {
		key_set(r, path);
		const char *home = g_get_home_dir();
		gsize hl = strlen(home);
		char *m = !strncmp(path, home, hl) && path[hl] == '/' ?
			g_strdup_printf("Imported as ~%s, and chosen for SSH logins", path + hl) :
			g_strdup_printf("Imported as %s, and chosen for SSH logins", path);
		window_toast(r->a, m);
		g_free(m);
	} else {
		char *m = g_strdup_printf("The key could not be saved: %s",
					  e ? e->message : "no room in ~/.ssh");
		window_toast(r->a, m);
		g_free(m);
	}
	g_clear_error(&e);
	g_free(line);
	g_free(comment);
	g_free(pubpath);
	g_free(path);
done:
	if (key) {
		memset(key, 0, strlen(key));
		dmc_ssh_free_string(key);
	}
	dmc_ssh_free_string(pub);
	memset(text, 0, strlen(text));
	g_free(text);
	g_free(name);
}

static void ppk_chosen(GObject *src, GAsyncResult *res, gpointer data)
{
	Remote *r = data;
	GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
	if (!f)
		return;
	char *p = g_file_get_path(f);
	char *text = NULL;
	if (p && g_file_get_contents(p, &text, NULL, NULL) && dmc_ssh_key_encrypted(text)) {
		char *name = g_path_get_basename(p);
		ask_passphrase(r, name, NULL, import_with, g_strdup(p), g_free, FALSE);
		g_free(name);
	} else if (p) {
		import_with(r, NULL, p);
	}
	if (text) {
		memset(text, 0, strlen(text));
		g_free(text);
	}
	g_free(p);
	g_object_unref(f);
}

static void import_clicked(GtkButton *b, gpointer data)
{
	Remote *r = data;
	gtk_menu_button_popdown(GTK_MENU_BUTTON(r->login));
	(void)b;
	GtkFileDialog *d = key_dialog("Import a PuTTY Key", TRUE);
	gtk_file_dialog_open(d, r->a->window, NULL, ppk_chosen, r);
	g_object_unref(d);
}

/* the menu's button: the way to log in, the key file, the agent's
   socket, and a .ppk's import */
static GtkWidget *login_menu(Remote *r)
{
	App *a = r->a;
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_set_margin_start(box, 6);
	gtk_widget_set_margin_end(box, 6);
	gtk_widget_set_margin_top(box, 6);
	gtk_widget_set_margin_bottom(box, 6);

	GtkWidget *l = gtk_label_new("Log in to SSH servers with");
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_widget_add_css_class(l, "heading");
	gtk_box_append(GTK_BOX(box), l);
	const char *names[] = { "My keys and agent, as ssh does",
				"The agent's keys only", "A key file", NULL };
	r->auth = gtk_drop_down_new_from_strings(names);
	guint i;
	for (i = 0; i < G_N_ELEMENTS(hows); i++)
		if (!strcmp(a->cfg.remote_auth, hows[i]))
			gtk_drop_down_set_selected(GTK_DROP_DOWN(r->auth), i);
	g_signal_connect(r->auth, "notify::selected", G_CALLBACK(how_changed), r);
	gtk_box_append(GTK_BOX(box), r->auth);

	l = gtk_label_new("Key file (OpenSSH's, PEM or PuTTY's .ppk)");
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_widget_add_css_class(l, "dim-label");
	gtk_box_append(GTK_BOX(box), l);
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	r->key_label = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(r->key_label), 0);
	gtk_label_set_ellipsize(GTK_LABEL(r->key_label), PANGO_ELLIPSIZE_MIDDLE);
	gtk_widget_set_hexpand(r->key_label, TRUE);
	gtk_box_append(GTK_BOX(row), r->key_label);
	GtkWidget *b = gtk_button_new_with_label("Choose…");
	g_signal_connect(b, "clicked", G_CALLBACK(choose_key), r);
	gtk_box_append(GTK_BOX(row), b);
	gtk_box_append(GTK_BOX(box), row);
	key_shown(r);

	b = gtk_button_new_with_label("Import a PuTTY Key…");
	gtk_widget_set_tooltip_text(b, "A .ppk saved in ~/.ssh in OpenSSH's form, "
				    "with the same passphrase, and chosen");
	g_signal_connect(b, "clicked", G_CALLBACK(import_clicked), r);
	gtk_box_append(GTK_BOX(box), b);

	l = gtk_label_new("Agent's socket");
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_widget_add_css_class(l, "dim-label");
	gtk_box_append(GTK_BOX(box), l);
	r->agent = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(r->agent), "Empty: SSH_AUTH_SOCK's");
	gtk_editable_set_text(GTK_EDITABLE(r->agent), a->cfg.remote_agent);
	g_signal_connect(r->agent, "changed", G_CALLBACK(agent_changed), r);
	gtk_box_append(GTK_BOX(box), r->agent);
	r->agent_state = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(r->agent_state), 0);
	gtk_widget_add_css_class(r->agent_state, "dim-label");
	gtk_widget_add_css_class(r->agent_state, "caption");
	gtk_box_append(GTK_BOX(box), r->agent_state);
	agent_shown(r);

	GtkWidget *pop = gtk_popover_new();
	gtk_popover_set_child(GTK_POPOVER(pop), box);
	r->login = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(r->login), "dialog-password-symbolic");
	gtk_widget_set_tooltip_text(r->login, "How SSH logs in: keys, the agent, a PuTTY key");
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(r->login), pop);
	return r->login;
}

static int proto_index(const char *name)
{
	guint i;
	for (i = 0; i < G_N_ELEMENTS(protos); i++)
		if (name && !g_ascii_strcasecmp(name, dmc_net_name(protos[i])))
			return (int)i;
	return 0;
}

GtkWidget *remote_page_new(App *a)
{
	Remote *r = g_new0(Remote, 1);
	R = r;
	r->a = a;
	r->pending = g_byte_array_new();

	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_margin_start(bar, 6);
	gtk_widget_set_margin_end(bar, 6);
	gtk_widget_set_margin_top(bar, 6);
	gtk_widget_set_margin_bottom(bar, 6);

	const char *names[] = { "SSH", "Telnet", "rlogin", NULL };
	r->proto = gtk_drop_down_new_from_strings(names);
	gtk_widget_set_tooltip_text(r->proto, "How to log in");
	gtk_box_append(GTK_BOX(bar), r->proto);

	r->host = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(r->host), "Server");
	gtk_widget_set_hexpand(r->host, TRUE);
	gtk_box_append(GTK_BOX(bar), r->host);

	r->port = gtk_entry_new();
	gtk_entry_set_input_purpose(GTK_ENTRY(r->port), GTK_INPUT_PURPOSE_DIGITS);
	gtk_editable_set_width_chars(GTK_EDITABLE(r->port), 6);
	gtk_editable_set_max_width_chars(GTK_EDITABLE(r->port), 6);
	gtk_widget_set_tooltip_text(r->port, "Port (empty: the protocol's)");
	gtk_box_append(GTK_BOX(bar), r->port);

	r->user = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(r->user), "User");
	gtk_editable_set_width_chars(GTK_EDITABLE(r->user), 12);
	gtk_box_append(GTK_BOX(bar), r->user);
	gtk_box_append(GTK_BOX(bar), login_menu(r));

	r->go = gtk_button_new_with_label("Connect");
	g_signal_connect(r->go, "clicked", G_CALLBACK(connect_clicked), r);
	g_signal_connect(r->host, "activate", G_CALLBACK(connect_clicked), r);
	gtk_box_append(GTK_BOX(bar), r->go);
	gtk_box_append(GTK_BOX(box), bar);

	/* what was used last */
	gtk_drop_down_set_selected(GTK_DROP_DOWN(r->proto),
				   (guint)proto_index(a->cfg.remote_proto));
	gtk_editable_set_text(GTK_EDITABLE(r->host), a->cfg.remote_host);
	if (a->cfg.remote_port > 0) {
		char ps[16];
		g_snprintf(ps, sizeof(ps), "%d", a->cfg.remote_port);
		gtk_editable_set_text(GTK_EDITABLE(r->port), ps);
	}
	gtk_editable_set_text(GTK_EDITABLE(r->user), a->cfg.remote_user);
	g_signal_connect(r->proto, "notify::selected", G_CALLBACK(proto_changed), r);
	proto_changed(NULL, NULL, r);

	r->term = termview_new(committed, r);
	termview_set_profile(r->term, a->cfg.remote_term);
	gtk_box_append(GTK_BOX(box), r->term);

	GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_margin_start(foot, 8);
	gtk_widget_set_margin_end(foot, 6);
	gtk_widget_set_margin_top(foot, 4);
	gtk_widget_set_margin_bottom(foot, 4);
	r->status = gtk_label_new("Not connected");
	gtk_label_set_xalign(GTK_LABEL(r->status), 0);
	gtk_widget_set_hexpand(r->status, TRUE);
	gtk_widget_add_css_class(r->status, "dim-label");
	gtk_box_append(GTK_BOX(foot), r->status);
	/* the kind of terminal: the far end is told it when connecting */
	r->type = term_types_new(a->cfg.remote_term);
	g_signal_connect(r->type, "notify::selected", G_CALLBACK(type_changed), r);
	gtk_box_append(GTK_BOX(foot), r->type);
	gtk_box_append(GTK_BOX(box), foot);
	controls(r);
	return box;
}

void remote_stop(App *a)
{
	(void)a;
	if (R && R->running)
		ended(R, NULL);
}
