/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * modem.c - dmodemd started and stopped, its pty opened, and the bytes
 *           between the pty, libdmodemctl's session and the terminal
 *
 * dmodemd runs as the user: it makes its pty and says where it is
 * ("Use `/dev/pts/3' as modem device"), and the app opens that.  With
 * the "device" line nothing is started and the given device is opened.
 */
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <glib-unix.h>

#include "app.h"

typedef struct Waiting {
	ResponseFn fn;
	gpointer data;
} Waiting;

#define LOG_LINES 5000

void app_log(App *a, const char *line)
{
	GtkTextIter end;
	gtk_text_buffer_get_end_iter(a->log, &end);
	gtk_text_buffer_insert(a->log, &end, line, -1);
	gtk_text_buffer_insert(a->log, &end, "\n", 1);
	int n = gtk_text_buffer_get_line_count(a->log);
	if (n > LOG_LINES) {
		GtkTextIter s, e;
		gtk_text_buffer_get_start_iter(a->log, &s);
		gtk_text_buffer_get_iter_at_line(a->log, &e, n - LOG_LINES);
		gtk_text_buffer_delete(a->log, &s, &e);
	}
}

static int64_t now_ms(void)
{
	return g_get_monotonic_time() / 1000;
}

static void flush(App *a)
{
	while (a->fd >= 0 && a->pending->len) {
		ssize_t n = write(a->fd, a->pending->data, a->pending->len);
		if (n > 0) {
			g_byte_array_remove_range(a->pending, 0, (guint)n);
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		break;          /* EAGAIN: the next tick */
	}
}

void modem_pump(App *a)
{
	char buf[4096];
	int n;
	dmc_poll(a->s, now_ms());
	while ((n = dmc_output(a->s, buf, sizeof(buf))) > 0)
		g_byte_array_append(a->pending, (guint8 *)buf, (guint)n);
	flush(a);
	while ((n = dmc_terminal(a->s, buf, sizeof(buf))) > 0)
		termview_feed(a->term, buf, (int)n);
	dmc_event ev;
	while (dmc_next_event(a->s, &ev)) {
		if (ev.type == DMC_EV_RESPONSE) {
			Waiting *w = g_hash_table_lookup(a->waiting,
							 GINT_TO_POINTER(ev.tag));
			if (w) {
				Waiting copy = *w;
				g_hash_table_remove(a->waiting,
						    GINT_TO_POINTER(ev.tag));
				if (copy.fn)
					copy.fn(a, &ev, copy.data);
			}
		} else {
			window_event(a, &ev);
		}
	}
}

static gboolean tick(gpointer data)
{
	modem_pump(data);
	return G_SOURCE_CONTINUE;
}

static void close_dte(App *a)
{
	if (a->fd_watch) {
		g_source_remove(a->fd_watch);
		a->fd_watch = 0;
	}
	if (a->fd >= 0) {
		close(a->fd);
		a->fd = -1;
	}
	g_byte_array_set_size(a->pending, 0);
	g_hash_table_remove_all(a->waiting);
	if (dmc_state(a->s) != DMC_CLOSED) {
		dmc_close(a->s);
		modem_pump(a);
		window_closed(a);
	}
}

static gboolean readable(gint fd, GIOCondition cond, gpointer data)
{
	App *a = data;
	char buf[4096];
	(void)cond;
	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf));
		if (n > 0) {
			dmc_feed(a->s, buf, (int)n);
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == EAGAIN)
			break;
		/* EOF, or EIO when dmodemd closed its end */
		a->fd_watch = 0;
		app_log(a, "The modem's device closed.");
		close_dte(a);
		return G_SOURCE_REMOVE;
	}
	modem_pump(a);
	return G_SOURCE_CONTINUE;
}

static gboolean open_dte(App *a, const char *path)
{
	int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		char *m = g_strdup_printf("Can't open %s: %s", path,
					  g_strerror(errno));
		app_log(a, m);
		window_toast(a, m);
		g_free(m);
		return FALSE;
	}
	struct termios t;
	if (!tcgetattr(fd, &t)) {
		cfmakeraw(&t);
		cfsetispeed(&t, B115200);
		cfsetospeed(&t, B115200);
		t.c_cflag |= CLOCAL | CREAD;
		tcsetattr(fd, TCSANOW, &t);
	}
	a->fd = fd;
	g_free(a->device);
	a->device = g_strdup(path);
	a->fd_watch = g_unix_fd_add(fd, G_IO_IN | G_IO_HUP | G_IO_ERR,
				    readable, a);
	dmc_open(a->s, now_ms());
	window_opened(a);
	/* the user's own commands first */
	if (*a->cfg.init)
		modem_command(a, a->cfg.init, 0, NULL, NULL);
	modem_pump(a);
	return TRUE;
}

/*
 *    dmodemd
 */

static char *find_dmodemd(const Config *c)
{
	if (*c->dmodemd)
		return g_strdup(c->dmodemd);
	char *p = g_find_program_in_path("dmodemd");
	if (p)
		return p;
	static const char *const places[] = {
		"/usr/local/sbin/dmodemd", "/usr/sbin/dmodemd",
		"/usr/local/bin/dmodemd", "/app/bin/dmodemd", NULL
	};
	for (int i = 0; places[i]; i++)
		if (g_file_test(places[i], G_FILE_TEST_IS_EXECUTABLE))
			return g_strdup(places[i]);
	return NULL;
}

typedef struct Reader {
	App *a;
	GDataInputStream *in;
} Reader;

static void read_line(Reader *r);

static void line_read(GObject *src, GAsyncResult *res, gpointer data)
{
	Reader *r = data;
	gsize len;
	GError *e = NULL;
	char *line = g_data_input_stream_read_line_finish_utf8(
		G_DATA_INPUT_STREAM(src), res, &len, &e);
	if (!line) {
		/* the end of dmodemd's output (or cancelled: the app may be
		   gone, so nothing of it is touched) */
		if (e)
			g_error_free(e);
		g_object_unref(r->in);
		g_free(r);
		return;
	}
	App *a = r->a;
	app_log(a, line);
	char dev[512];
	if (dmc_daemon_ready(line, dev, sizeof(dev)) && a->fd < 0)
		open_dte(a, dev);
	else if (dmc_daemon_error(line) && a->fd < 0 &&
		 !strstr(line, "group") && !strstr(line, "symbolic link"))
		window_toast(a, line + 7);
	g_free(line);
	read_line(r);
}

static void read_line(Reader *r)
{
	g_data_input_stream_read_line_async(r->in, G_PRIORITY_DEFAULT,
					    r->a->cancel, line_read, r);
}

static void watch(App *a, GInputStream *s)
{
	Reader *r = g_new0(Reader, 1);
	r->a = a;
	r->in = g_data_input_stream_new(s);
	read_line(r);
}

static void exited(GObject *src, GAsyncResult *res, gpointer data)
{
	GSubprocess *p = G_SUBPROCESS(src);
	GError *e = NULL;
	if (!g_subprocess_wait_finish(p, res, &e)) {
		/* cancelled: the app is closing */
		g_error_free(e);
		g_object_unref(p);
		return;
	}
	App *a = data;
	char *m;
	if (g_subprocess_get_if_signaled(p))
		m = g_strdup_printf("dmodemd stopped (signal %d).",
				    g_subprocess_get_term_sig(p));
	else
		m = g_strdup_printf("dmodemd stopped (exit status %d).",
				    g_subprocess_get_exit_status(p));
	app_log(a, m);
	if (a->proc == p) {
		g_clear_object(&a->proc);
		close_dte(a);
		if (!g_subprocess_get_if_signaled(p) ||
		    g_subprocess_get_term_sig(p) != SIGTERM)
			window_toast(a, m);
	}
	g_free(m);
	g_object_unref(p);
	window_state(a);
}

static void setenv_if(GSubprocessLauncher *l, const char *name,
		      gboolean set, const char *value)
{
	if (set)
		g_subprocess_launcher_setenv(l, name, value, TRUE);
}

void modem_start(App *a)
{
	Config *c = &a->cfg;
	if (modem_running(a))
		return;
	if (!g_strcmp0(c->line, "device")) {
		open_dte(a, c->device);
		window_state(a);
		return;
	}
	char *exe = find_dmodemd(c);
	if (!exe) {
		window_toast(a, "dmodemd was not found: install D-Modem, or "
			     "set its path in the line settings");
		return;
	}
	if (!g_strcmp0(c->line, "sip") &&
	    (!*c->sip_user || !*c->sip_server)) {
		window_toast(a, "Set up the SIP account first");
		prefs_show(a);
		g_free(exe);
		return;
	}
	GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(argv, exe);
	if (*c->country) {
		g_ptr_array_add(argv, g_strdup("-c"));
		g_ptr_array_add(argv, g_strdup(c->country));
	}
	/* (the pty in the user's own group: the default, dialout, would
	   need root) */
	struct group *gr = getgrgid(getgid());
	if (gr) {
		g_ptr_array_add(argv, g_strdup("-g"));
		g_ptr_array_add(argv, g_strdup(gr->gr_name));
	}
	if (c->debug > 0)
		g_ptr_array_add(argv, g_strdup_printf("-d%d", c->debug));
	if (!g_strcmp0(c->line, "usb")) {
		g_ptr_array_add(argv, g_strdup_printf("--usb=%s", c->usb));
	} else if (!g_strcmp0(c->line, "alsa")) {
		g_ptr_array_add(argv, g_strdup("-a"));
		g_ptr_array_add(argv, g_strdup(c->alsa));
	} else if (c->listen) {
		g_ptr_array_add(argv, g_strdup("-L"));
	}
	g_ptr_array_add(argv, NULL);

	GSubprocessLauncher *l = g_subprocess_launcher_new(
		G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	if (!g_strcmp0(c->line, "sip")) {
		char *login = g_strdup_printf("%s:%s@%s", c->sip_user,
					      c->sip_password, c->sip_server);
		g_subprocess_launcher_setenv(l, "SIP_LOGIN", login, TRUE);
		g_free(login);
	}
	setenv_if(l, "SIP_T38", !c->t38, "no");
	setenv_if(l, "SIP_V152", !c->v152, "no");
	setenv_if(l, "SIP_V150", c->v150, "yes");
	setenv_if(l, "SIP_SRTP", *c->srtp, c->srtp);
	/* its log files where the user may write them */
	char *dir = g_build_filename(g_get_user_cache_dir(), "dmodem-gui", NULL);
	g_mkdir_with_parents(dir, 0700);
	g_subprocess_launcher_set_cwd(l, dir);
	g_free(dir);

	GError *e = NULL;
	a->proc = g_subprocess_launcher_spawnv(l, (const char *const *)argv->pdata,
					       &e);
	g_object_unref(l);
	char *shown = g_strjoinv(" ", (char **)argv->pdata);
	g_ptr_array_unref(argv);
	if (!a->proc) {
		char *m = g_strdup_printf("Can't start dmodemd: %s", e->message);
		window_toast(a, m);
		app_log(a, m);
		g_free(m);
		g_error_free(e);
		g_free(shown);
		window_state(a);
		return;
	}
	char *m = g_strdup_printf("$ %s", shown);
	app_log(a, m);
	g_free(m);
	g_free(shown);
	watch(a, g_subprocess_get_stderr_pipe(a->proc));
	watch(a, g_subprocess_get_stdout_pipe(a->proc));
	g_subprocess_wait_async(g_object_ref(a->proc), a->cancel, exited, a);
	window_state(a);
}

void modem_stop(App *a)
{
	if (a->proc) {
		/* hang up first, so the call ends cleanly */
		if (a->fd >= 0)
			dmc_hangup(a->s);
		modem_pump(a);
		close_dte(a);
		g_subprocess_send_signal(a->proc, SIGTERM);
	} else {
		close_dte(a);
	}
	window_state(a);
}

gboolean modem_running(App *a)
{
	return a->proc != NULL || a->fd >= 0;
}

void modem_type(App *a, const char *data, gsize n)
{
	if (a->fd < 0)
		return;
	dmc_type(a->s, data, (int)n);
	modem_pump(a);
}

void modem_wait(App *a, int tag, ResponseFn fn, gpointer data)
{
	if (tag > 0) {
		Waiting *w = g_new0(Waiting, 1);
		w->fn = fn;
		w->data = data;
		g_hash_table_insert(a->waiting, GINT_TO_POINTER(tag), w);
		modem_pump(a);
	}
}

int modem_command(App *a, const char *cmd, int flags, ResponseFn fn,
		  gpointer data)
{
	int tag = dmc_command(a->s, cmd, flags, 0);
	if (tag > 0) {
		Waiting *w = g_new0(Waiting, 1);
		w->fn = fn;
		w->data = data;
		g_hash_table_insert(a->waiting, GINT_TO_POINTER(tag), w);
		modem_pump(a);
	}
	return tag;
}

/* the ticks the session's timers need */
void modem_init(App *a);
void modem_init(App *a)
{
	a->s = dmc_session_new();
	a->fd = -1;
	a->pending = g_byte_array_new();
	a->waiting = g_hash_table_new_full(g_direct_hash, g_direct_equal,
					   NULL, g_free);
	a->cancel = g_cancellable_new();
	a->tick = g_timeout_add(50, tick, a);
}
