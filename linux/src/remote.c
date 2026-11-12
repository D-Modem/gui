/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * remote.c - the Remote page: a login to a server by SSH, Telnet or
 *            rlogin, in a terminal of its own, beside the modem's
 *
 * Telnet and rlogin are a TCP connection made here and libdmodemctl's
 * dmc_net for the protocol.  SSH is the system's ssh in a pty of this
 * page's own, so that its host keys, agent and config are the user's.
 * Either way the terminal only shows bytes and gives what is typed.
 */
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <pty.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#include <libutil.h>
#else
#include <util.h>
#endif

#include <glib-unix.h>

#include "app.h"

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
	/* SSH: the pty's master, ssh's pid */
	int fd;
	GPid pid;
	guint fd_watch, child_watch, size_tick;
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
	if (r->fd_watch)
		g_source_remove(r->fd_watch);
	r->fd_watch = 0;
	if (r->fd >= 0)
		close(r->fd);
	r->fd = -1;
	if (r->pid > 0)
		kill(r->pid, SIGHUP);
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
 *    SSH: the system's ssh in a pty
 */

static gboolean pty_readable(int fd, GIOCondition c, gpointer data)
{
	Remote *r = data;
	char buf[8192];
	ssize_t n = read(fd, buf, sizeof(buf));
	if (n > 0) {
		show(r, buf, n);
		return G_SOURCE_CONTINUE;
	}
	if (n < 0 && (errno == EAGAIN || errno == EINTR))
		return G_SOURCE_CONTINUE;
	/* (EIO: ssh has gone and the pty is shut; its exit says why) */
	r->fd_watch = 0;
	return G_SOURCE_REMOVE;
}

static void child_exited(GPid pid, int status, gpointer data)
{
	Remote *r = data;
	g_spawn_close_pid(pid);
	r->child_watch = 0;
	if (r->pid != pid)
		return;
	r->pid = 0;
	/* what is left in the pty first */
	if (r->fd >= 0) {
		char buf[8192];
		ssize_t n;
		while ((n = read(r->fd, buf, sizeof(buf))) > 0)
			show(r, buf, n);
	}
	char *why = WIFEXITED(status) && WEXITSTATUS(status) ?
		g_strdup_printf("ssh exited with status %d", WEXITSTATUS(status)) :
		g_strdup("Connection closed");
	ended(r, why);
	g_free(why);
}

static void set_pty_size(Remote *r)
{
	struct winsize ws = { 0 };
	ws.ws_col = (unsigned short)r->cols;
	ws.ws_row = (unsigned short)r->rows;
	ioctl(r->fd, TIOCSWINSZ, &ws);
}

static gboolean start_ssh(Remote *r, const char *host, int port,
			  const char *user)
{
	char *ssh = g_find_program_in_path("ssh");
	if (!ssh) {
		window_toast(r->a, "ssh was not found: install OpenSSH's client");
		return FALSE;
	}
	char ps[16];
	g_snprintf(ps, sizeof(ps), "%d", port);
	struct winsize ws = { 0 };
	ws.ws_col = (unsigned short)r->cols;
	ws.ws_row = (unsigned short)r->rows;
	int fd;
	pid_t pid = forkpty(&fd, NULL, NULL, &ws);
	if (pid < 0) {
		g_free(ssh);
		window_toast(r->a, "Could not make a pty for ssh");
		return FALSE;
	}
	if (pid == 0) {
		/* the child: ssh, with VTE's terminal type */
		const char *argv[10];
		int i = 0;
		argv[i++] = "ssh";
		argv[i++] = "-p";
		argv[i++] = ps;
		if (*user) {
			argv[i++] = "-l";
			argv[i++] = user;
		}
		argv[i++] = "--";
		argv[i++] = host;
		argv[i] = NULL;
		setenv("TERM", "xterm-256color", 1);
		execv(ssh, (char **)argv);
		_exit(127);
	}
	g_free(ssh);
	g_unix_set_fd_nonblocking(fd, TRUE, NULL);
	r->fd = fd;
	r->pid = pid;
	r->fd_watch = g_unix_fd_add(fd, G_IO_IN | G_IO_HUP | G_IO_ERR,
				    pty_readable, r);
	r->child_watch = g_child_watch_add(pid, child_exited, r);
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
	} else if (r->fd >= 0) {
		set_pty_size(r);
	}
	return G_SOURCE_CONTINUE;
}

static void committed(VteTerminal *t, char *text, guint size, gpointer data)
{
	Remote *r = data;
	(void)t;
	if (!r->running)
		return;
	if (r->fd >= 0) {
		/* (a pty takes small writes whole) */
		ssize_t w = write(r->fd, text, size);
		(void)w;
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
		set_status(r, "Connected by SSH");
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
	r->fd = -1;
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
