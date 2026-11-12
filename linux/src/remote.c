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
 * they are the user's.  Either way the terminal only shows bytes and
 * gives what is typed.
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
	VteTerminal *term;
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
	vte_terminal_feed(r->term, data, n);
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

/* what's tried, in OpenSSH's order: keys and the agent, then the
   server's questions or a password (three times at most) */
enum { TRIED_KEYS = 1 };

static void ssh_auth(Remote *r, int methods, const char *why)
{
	Ssh *ssh = r->ssh;
	if ((methods & DMC_SSH_AUTH_PUBLICKEY) && !(r->tried & TRIED_KEYS)) {
		r->tried |= TRIED_KEYS;
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
		r->shell = dmc_ssh_shell(ssh->s, "xterm-256color", r->cols, r->rows, 0);
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
	Ssh *ssh = g_new0(Ssh, 1);
	ssh->s = s;
	r->ssh = ssh;
	r->shell = 0;
	r->tried = 0;
	r->asks = 0;
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
	int c = (int)vte_terminal_get_column_count(r->term);
	int w = (int)vte_terminal_get_row_count(r->term);
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

static void committed(VteTerminal *t, char *text, guint size, gpointer data)
{
	Remote *r = data;
	(void)t;
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

	vte_terminal_reset(r->term, TRUE, TRUE);
	r->cols = (int)vte_terminal_get_column_count(r->term);
	r->rows = (int)vte_terminal_get_row_count(r->term);
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
		r->net = dmc_net_new(proto, user, "xterm-256color", r->cols,
				     r->rows);
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
	gtk_widget_grab_focus(GTK_WIDGET(r->term));
	g_free(host);
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

	r->term = VTE_TERMINAL(vte_terminal_new());
	vte_terminal_set_scrollback_lines(r->term, 5000);
	vte_terminal_set_backspace_binding(r->term, VTE_ERASE_ASCII_DELETE);
	vte_terminal_set_size(r->term, 80, 24);
	PangoFontDescription *f = pango_font_description_from_string("Monospace 11");
	vte_terminal_set_font(r->term, f);
	pango_font_description_free(f);
	GdkRGBA fg, bg;
	gdk_rgba_parse(&fg, "#e8f0ff");
	gdk_rgba_parse(&bg, "#0a0f1c");
	vte_terminal_set_colors(r->term, &fg, &bg, NULL, 0);
	gtk_widget_set_vexpand(GTK_WIDGET(r->term), TRUE);
	gtk_widget_set_hexpand(GTK_WIDGET(r->term), TRUE);
	g_signal_connect(r->term, "commit", G_CALLBACK(committed), r);
	GtkWidget *scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
				      GTK_WIDGET(r->term));
	gtk_widget_set_vexpand(scroll, TRUE);
	gtk_box_append(GTK_BOX(box), scroll);

	r->status = gtk_label_new("Not connected");
	gtk_label_set_xalign(GTK_LABEL(r->status), 0);
	gtk_widget_add_css_class(r->status, "dim-label");
	gtk_widget_set_margin_start(r->status, 8);
	gtk_widget_set_margin_top(r->status, 4);
	gtk_widget_set_margin_bottom(r->status, 4);
	gtk_box_append(GTK_BOX(box), r->status);
	controls(r);
	return box;
}

void remote_stop(App *a)
{
	(void)a;
	if (R && R->running)
		ended(R, NULL);
}
