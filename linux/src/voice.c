/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * voice.c - the Phone page: voice calls dialled and answered, their
 *           sound on the sound card, the keypad's DTMF; the answering
 *           machine (off until the user turns it on), its greeting and
 *           the messages it took, kept as WAV files in
 *           ~/.local/share/dmodem-gui/voicemail
 */
#include <stdlib.h>
#include <string.h>

#include <glib/gstdio.h>

#include "app.h"

/* the longest greeting recorded (s) */
#define GREETING_MAX_S 30

typedef struct Voice {
	GtkWidget *status, *number, *call_btn, *answer_btn, *message_btn;
	GtkWidget *pickup_btn, *hangup_btn, *keypad, *mute_btn, *listen_btn;
	GtkWidget *vm_switch, *vm_rings, *greeting_row, *record_btn;
	GtkWidget *greeting_play, *greeting_clear;
	AdwPreferencesGroup *messages;
	GPtrArray *message_rows;
	GtkWidget *messages_empty;
	guint fast;             /* the 20 ms tick of a call */
	int last_state;
	gboolean muted, listening, listen_on;
	/* what plays (a message, the greeting), and what is recorded */
	int16_t *play;
	int play_n, play_pos;
	guint play_end;
	GArray *recording;
	/* the greeting, 8 kHz */
	int16_t *greeting;
	int greeting_n;
} Voice;

/*
 *    files
 */

static char *data_dir(void)
{
	return g_build_filename(g_get_user_data_dir(), "dmodem-gui", NULL);
}

static char *messages_dir(void)
{
	return g_build_filename(g_get_user_data_dir(), "dmodem-gui",
				"voicemail", NULL);
}

static char *greeting_path(void)
{
	return g_build_filename(g_get_user_data_dir(), "dmodem-gui",
				"greeting.wav", NULL);
}

/* samples as a WAV file at path */
static gboolean write_wav(const char *path, const int16_t *pcm, int n,
			  GError **e)
{
	char *dir = g_path_get_dirname(path);
	g_mkdir_with_parents(dir, 0700);
	g_free(dir);
	gsize len = 44 + (gsize)n * 2;
	char *b = g_malloc(len);
	dmc_wav_header(b, n);
	for (int i = 0; i < n; i++) {
		guint16 v = GUINT16_TO_LE((guint16)pcm[i]);
		memcpy(b + 44 + i * 2, &v, 2);
	}
	gboolean ok = g_file_set_contents_full(path, b, (gssize)len,
					       G_FILE_SET_CONTENTS_CONSISTENT,
					       0600, e);
	g_free(b);
	return ok;
}

/* a WAV file's samples at 8 kHz: how many (*pcm to dmc_free()), -1 */
static int read_wav(const char *path, int16_t **pcm)
{
	char *b;
	gsize len;
	*pcm = NULL;
	if (!g_file_get_contents(path, &b, &len, NULL))
		return -1;
	int n = len > G_MAXINT ? -1 : dmc_wav_decode(b, (int)len, pcm);
	g_free(b);
	return n;
}

/*
 *    sound
 */

static int call_pull(App *a, int16_t *pcm, int n)
{
	return dmc_voice_read(a->s, pcm, n);
}

static void call_push(App *a, const int16_t *pcm, int n)
{
	if (!a->voice->muted)
		dmc_voice_write(a->s, pcm, n);
}

static gboolean play_done(gpointer data)
{
	App *a = data;
	Voice *v = a->voice;
	v->play_end = 0;
	audio_stop(a);
	g_clear_pointer(&v->play, dmc_free);
	v->play_n = v->play_pos = 0;
	voice_state(a);
	return G_SOURCE_REMOVE;
}

static int play_pull(App *a, int16_t *pcm, int n)
{
	Voice *v = a->voice;
	int k = v->play_n - v->play_pos;
	if (k > n)
		k = n;
	if (k > 0) {
		memcpy(pcm, v->play + v->play_pos, (size_t)k * 2);
		v->play_pos += k;
	}
	/* (stopped from the main loop, not inside the sound's callback) */
	if (v->play_pos >= v->play_n && !v->play_end)
		v->play_end = g_timeout_add(300, play_done, a);
	return k;
}

static void record_push(App *a, const int16_t *pcm, int n)
{
	Voice *v = a->voice;
	if (!v->recording)
		return;
	int room = GREETING_MAX_S * DMC_VOICE_RATE - (int)v->recording->len;
	if (n > room)
		n = room;
	if (n > 0)
		g_array_append_vals(v->recording, pcm, (guint)n);
}

static void stop_sound(App *a)
{
	Voice *v = a->voice;
	if (v->play_end) {
		g_source_remove(v->play_end);
		v->play_end = 0;
	}
	audio_stop(a);
	g_clear_pointer(&v->play, dmc_free);
	v->play_n = v->play_pos = 0;
	if (v->recording) {
		g_array_unref(v->recording);
		v->recording = NULL;
	}
}

/* pcm (taken: dmc_free()d when done) played */
static void play(App *a, int16_t *pcm, int n)
{
	Voice *v = a->voice;
	stop_sound(a);
	v->play = pcm;
	v->play_n = n;
	v->play_pos = 0;
	if (!audio_start(a, play_pull, NULL)) {
		window_toast(a, "No sound: D-Modem was built without "
			     "PulseAudio");
		stop_sound(a);
	}
	voice_state(a);
}

/*
 *    the answering machine's greeting and messages
 */

static void load_greeting(App *a)
{
	Voice *v = a->voice;
	char *p = greeting_path();
	g_clear_pointer(&v->greeting, dmc_free);
	v->greeting_n = 0;
	int16_t *pcm;
	int n = read_wav(p, &pcm);
	if (n > 0) {
		v->greeting = pcm;
		v->greeting_n = n;
	} else if (pcm) {
		dmc_free(pcm);
	}
	g_free(p);
}

void voice_configure(App *a)
{
	Voice *v = a->voice;
	dmc_voicemail_set(a->s, v->greeting, v->greeting_n, a->cfg.vm_max, 0);
	dmc_voicemail_auto(a->s, a->cfg.voicemail ? a->cfg.vm_rings : 0);
}

static void refresh_messages(App *a);

static void play_message(GtkWidget *w, gpointer data)
{
	App *a = data;
	const char *path = g_object_get_data(G_OBJECT(w), "path");
	int16_t *pcm;
	int n = read_wav(path, &pcm);
	if (n <= 0) {
		if (pcm)
			dmc_free(pcm);
		window_toast(a, "Can't read the message");
		return;
	}
	play(a, pcm, n);
}

static void delete_message(GtkWidget *w, gpointer data)
{
	App *a = data;
	const char *path = g_object_get_data(G_OBJECT(w), "path");
	stop_sound(a);
	if (g_remove(path) < 0)
		window_toast(a, "Can't delete the message");
	refresh_messages(a);
	voice_state(a);
}

static int newest_first(gconstpointer x, gconstpointer y)
{
	return -g_strcmp0(*(char *const *)x, *(char *const *)y);
}

/* the message list from the files: 20261111-120501_5551234.wav */
static void refresh_messages(App *a)
{
	Voice *v = a->voice;
	for (guint i = 0; i < v->message_rows->len; i++)
		adw_preferences_group_remove(v->messages,
					     g_ptr_array_index(v->message_rows, i));
	g_ptr_array_set_size(v->message_rows, 0);
	char *dir = messages_dir();
	GDir *d = g_dir_open(dir, 0, NULL);
	GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
	const char *n;
	while (d && (n = g_dir_read_name(d)))
		if (g_str_has_suffix(n, ".wav"))
			g_ptr_array_add(names, g_strdup(n));
	if (d)
		g_dir_close(d);
	g_ptr_array_sort(names, newest_first);
	for (guint i = 0; i < names->len; i++) {
		const char *name = g_ptr_array_index(names, i);
		char *path = g_build_filename(dir, name, NULL);
		int y, mo, da, h, mi, se;
		char who[64] = "";
		if (sscanf(name, "%4d%2d%2d-%2d%2d%2d_%63[^.]", &y, &mo, &da,
			   &h, &mi, &se, who) < 6) {
			g_free(path);
			continue;
		}
		GStatBuf st;
		long secs = !g_stat(path, &st) && st.st_size > 44 ?
			(long)((st.st_size - 44) / (2 * DMC_VOICE_RATE)) : 0;
		GtkWidget *row = adw_action_row_new();
		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
					      *who ? who : "Unknown caller");
		char *sub = g_strdup_printf("%04d-%02d-%02d %02d:%02d · %ld:%02ld",
					    y, mo, da, h, mi, secs / 60,
					    secs % 60);
		adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sub);
		g_free(sub);
		GtkWidget *pb = gtk_button_new_from_icon_name(
			"media-playback-start-symbolic");
		gtk_widget_set_tooltip_text(pb, "Play");
		gtk_widget_set_valign(pb, GTK_ALIGN_CENTER);
		gtk_widget_add_css_class(pb, "flat");
		g_object_set_data_full(G_OBJECT(pb), "path", g_strdup(path),
				       g_free);
		g_signal_connect(pb, "clicked", G_CALLBACK(play_message), a);
		GtkWidget *db = gtk_button_new_from_icon_name(
			"user-trash-symbolic");
		gtk_widget_set_tooltip_text(db, "Delete");
		gtk_widget_set_valign(db, GTK_ALIGN_CENTER);
		gtk_widget_add_css_class(db, "flat");
		g_object_set_data_full(G_OBJECT(db), "path", path, g_free);
		g_signal_connect(db, "clicked", G_CALLBACK(delete_message), a);
		adw_action_row_add_suffix(ADW_ACTION_ROW(row), pb);
		adw_action_row_add_suffix(ADW_ACTION_ROW(row), db);
		adw_preferences_group_add(v->messages, row);
		g_ptr_array_add(v->message_rows, row);
	}
	gtk_widget_set_visible(v->messages_empty, v->message_rows->len == 0);
	g_ptr_array_unref(names);
	g_free(dir);
}

/* the message the answering machine took, kept */
static void save_message(App *a, const char *number)
{
	const int16_t *pcm;
	int n = dmc_voice_message(a->s, &pcm);
	if (n <= 0)
		return;
	GDateTime *now = g_date_time_new_now_local();
	char *stamp = g_date_time_format(now, "%Y%m%d-%H%M%S");
	g_date_time_unref(now);
	/* (the number as a file name allows: digits, + * #) */
	GString *who = g_string_new("");
	for (const char *p = number; *p && who->len < 32; p++)
		if (g_ascii_isalnum(*p) || *p == '+' || *p == '*' || *p == '#')
			g_string_append_c(who, *p);
	char *name = g_strdup_printf("%s_%s.wav", stamp, who->str);
	char *dir = messages_dir();
	char *path = g_build_filename(dir, name, NULL);
	GError *e = NULL;
	if (!write_wav(path, pcm, n, &e)) {
		char *m = g_strdup_printf("Can't keep the message: %s",
					  e->message);
		window_toast(a, m);
		g_free(m);
		g_error_free(e);
	} else {
		GNotification *nt = g_notification_new("New voice message");
		char *body = g_strdup_printf("From %s", *who->str ? who->str :
					     "an unknown caller");
		g_notification_set_body(nt, body);
		g_application_send_notification(G_APPLICATION(a->gapp),
						"voicemail", nt);
		g_object_unref(nt);
		g_free(body);
		app_log(a, "The answering machine took a message.");
	}
	g_free(path);
	g_free(dir);
	g_free(name);
	g_free(stamp);
	g_string_free(who, TRUE);
	refresh_messages(a);
}

/* the greeting: recorded from the microphone */
static void record_greeting(GtkWidget *w, gpointer data)
{
	App *a = data;
	Voice *v = a->voice;
	(void)w;
	if (v->recording) {
		/* stopped: kept */
		GArray *r = g_array_ref(v->recording);
		stop_sound(a);
		if (r->len > DMC_VOICE_RATE / 2) {
			char *p = greeting_path();
			GError *e = NULL;
			if (!write_wav(p, (int16_t *)r->data, (int)r->len, &e)) {
				window_toast(a, e->message);
				g_error_free(e);
			}
			g_free(p);
			load_greeting(a);
			voice_configure(a);
		}
		g_array_unref(r);
		voice_state(a);
		return;
	}
	stop_sound(a);
	v->recording = g_array_new(FALSE, FALSE, sizeof(int16_t));
	if (!audio_start(a, NULL, record_push)) {
		window_toast(a, "No sound: D-Modem was built without "
			     "PulseAudio");
		stop_sound(a);
	}
	voice_state(a);
}

static void play_greeting(GtkWidget *w, gpointer data)
{
	App *a = data;
	Voice *v = a->voice;
	(void)w;
	if (v->play) {
		stop_sound(a);
		voice_state(a);
		return;
	}
	if (!v->greeting_n)
		return;
	/* (play() frees it with dmc_free(), which is free()) */
	int16_t *copy = malloc((size_t)v->greeting_n * 2);
	if (!copy)
		return;
	memcpy(copy, v->greeting, (size_t)v->greeting_n * 2);
	play(a, copy, v->greeting_n);
}

static void clear_greeting(GtkWidget *w, gpointer data)
{
	App *a = data;
	(void)w;
	char *p = greeting_path();
	g_remove(p);
	g_free(p);
	load_greeting(a);
	voice_configure(a);
	voice_state(a);
}

static void greeting_chosen(GObject *src, GAsyncResult *res, gpointer data)
{
	App *a = data;
	GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
	if (!f)
		return;
	char *path = g_file_get_path(f);
	g_object_unref(f);
	int16_t *pcm;
	int n = path ? read_wav(path, &pcm) : -1;
	g_free(path);
	if (n <= 0) {
		if (n == 0)
			dmc_free(pcm);
		window_toast(a, "That isn't a WAV file D-Modem can play "
			     "(PCM, A-law or µ-law)");
		return;
	}
	char *p = greeting_path();
	GError *e = NULL;
	if (!write_wav(p, pcm, n, &e)) {
		window_toast(a, e->message);
		g_error_free(e);
	}
	g_free(p);
	dmc_free(pcm);
	load_greeting(a);
	voice_configure(a);
	voice_state(a);
}

static void choose_greeting(GtkWidget *w, gpointer data)
{
	App *a = data;
	(void)w;
	GtkFileDialog *d = gtk_file_dialog_new();
	gtk_file_dialog_set_title(d, "Greeting");
	GtkFileFilter *f = gtk_file_filter_new();
	gtk_file_filter_set_name(f, "WAV files");
	gtk_file_filter_add_suffix(f, "wav");
	GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
	g_list_store_append(filters, f);
	gtk_file_dialog_set_filters(d, G_LIST_MODEL(filters));
	g_object_unref(filters);
	g_object_unref(f);
	gtk_file_dialog_open(d, a->window, NULL, greeting_chosen, a);
	g_object_unref(d);
}

static void voicemail_switched(GObject *o, GParamSpec *ps, gpointer data)
{
	App *a = data;
	(void)ps;
	a->cfg.voicemail = adw_switch_row_get_active(ADW_SWITCH_ROW(o));
	config_save(&a->cfg);
	voice_configure(a);
}

static void rings_changed(GObject *o, GParamSpec *ps, gpointer data)
{
	App *a = data;
	(void)ps;
	a->cfg.vm_rings = (int)adw_spin_row_get_value(ADW_SPIN_ROW(o));
	config_save(&a->cfg);
	voice_configure(a);
}

/*
 *    calls
 */

static void dialled(App *a, const dmc_event *ev, gpointer data)
{
	(void)data;
	if (!ev->ok) {
		char *m = !strcmp(ev->result, "ERROR") ?
			g_strdup("The modem has no voice mode (D-Modem 1.1 "
				 "or later has)") :
			g_strdup_printf("%c%s", g_ascii_toupper(ev->result[0]),
					ev->result + 1);
		window_toast(a, m);
		g_free(m);
	}
}

static void call(GtkWidget *w, gpointer data)
{
	App *a = data;
	(void)w;
	const char *n = gtk_editable_get_text(GTK_EDITABLE(a->voice->number));
	if (!*n) {
		window_toast(a, "Enter a number to call");
		return;
	}
	stop_sound(a);
	modem_wait(a, dmc_voice_dial(a->s, n), dialled, NULL);
}

static void answer(GtkWidget *w, gpointer data)
{
	App *a = data;
	(void)w;
	stop_sound(a);
	modem_wait(a, dmc_voice_answer(a->s), dialled, NULL);
}

static void take_message(GtkWidget *w, gpointer data)
{
	App *a = data;
	(void)w;
	stop_sound(a);
	modem_wait(a, dmc_voicemail_answer(a->s), dialled, NULL);
}

static void pickup(GtkWidget *w, gpointer data)
{
	App *a = data;
	(void)w;
	dmc_voice_pickup(a->s);
	modem_pump(a);
}

static void hangup(GtkWidget *w, gpointer data)
{
	App *a = data;
	(void)w;
	dmc_hangup(a->s);
	modem_pump(a);
}

static void key(GtkWidget *w, gpointer data)
{
	App *a = data;
	const char *k = gtk_button_get_label(GTK_BUTTON(w));
	int st = dmc_state(a->s);
	if (st == DMC_VOICE) {
		dmc_voice_dtmf(a->s, k[0]);
	} else {
		GtkEditable *e = GTK_EDITABLE(a->voice->number);
		int pos = -1;
		gtk_editable_insert_text(e, k, 1, &pos);
	}
}

static void mute(GtkWidget *w, gpointer data)
{
	App *a = data;
	a->voice->muted = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
}

static void listen_toggled(GtkWidget *w, gpointer data)
{
	App *a = data;
	a->voice->listening = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
	voice_state(a);
}

static gboolean fast_tick(gpointer data)
{
	modem_pump(data);
	return G_SOURCE_CONTINUE;
}

/* the sound as the call needs it: both ways in a call; the caller (if
   listening) while the answering machine has the call */
static void call_sound(App *a, int st)
{
	Voice *v = a->voice;
	int was = v->last_state;
	gboolean in = st == DMC_VOICE || st == DMC_VOICEMAIL;
	gboolean listen = st == DMC_VOICEMAIL && v->listening;
	if (in && !v->fast) {
		v->fast = g_timeout_add(20, fast_tick, a);
	} else if (!in && st != DMC_HANGING_UP && v->fast) {
		g_source_remove(v->fast);
		v->fast = 0;
	}
	if (st == DMC_VOICE && was != DMC_VOICE) {
		stop_sound(a);
		v->listen_on = FALSE;
		if (!audio_start(a, call_pull, call_push))
			window_toast(a, "No sound: D-Modem was built without "
				     "PulseAudio");
	} else if (listen && !v->listen_on) {
		audio_start(a, call_pull, NULL);
		v->listen_on = TRUE;
	} else if (!listen && v->listen_on) {
		audio_stop(a);
		v->listen_on = FALSE;
	} else if (was == DMC_VOICE && st != DMC_VOICE) {
		audio_stop(a);
	}
	v->last_state = st;
}

void voice_state(App *a)
{
	Voice *v = a->voice;
	if (!v)
		return;
	int st = dmc_state(a->s);
	call_sound(a, st);
	const char *who = dmc_caller_number(a->s);
	char *status;
	switch (st) {
	case DMC_CLOSED:
		status = g_strdup("The modem isn't running");
		break;
	case DMC_RINGING:
		status = g_strdup_printf("Ringing%s%s", *who ? ": " : "", who);
		break;
	case DMC_VOICE:
		status = g_strdup("In a call");
		break;
	case DMC_VOICEMAIL:
		status = g_strdup_printf("Taking a message%s%s",
					 *who ? " from " : "", who);
		break;
	default:
		status = g_strdup(dmc_state_name(st));
		break;
	}
	adw_action_row_set_subtitle(ADW_ACTION_ROW(v->status), status);
	g_free(status);
	gboolean idle = st == DMC_IDLE;
	gboolean busy = v->play || v->recording;
	gtk_widget_set_sensitive(v->call_btn, idle);
	gtk_widget_set_sensitive(v->answer_btn, st == DMC_RINGING);
	gtk_widget_set_sensitive(v->message_btn, st == DMC_RINGING);
	gtk_widget_set_visible(v->pickup_btn, st == DMC_VOICEMAIL);
	gtk_widget_set_visible(v->listen_btn, st == DMC_VOICEMAIL);
	gtk_widget_set_sensitive(v->hangup_btn,
				 st == DMC_VOICE || st == DMC_VOICEMAIL ||
				 (st == DMC_DIALING || st == DMC_ANSWERING));
	gtk_widget_set_sensitive(v->mute_btn, st == DMC_VOICE);
	gtk_widget_set_sensitive(v->keypad, st == DMC_VOICE || idle ||
				 st == DMC_CLOSED);
	gtk_button_set_label(GTK_BUTTON(v->record_btn),
			     v->recording ? "Stop" : "Record");
	gtk_widget_set_sensitive(v->record_btn, !v->play &&
				 audio_available() &&
				 st != DMC_VOICE && st != DMC_VOICEMAIL);
	gtk_button_set_icon_name(GTK_BUTTON(v->greeting_play),
				 v->play ? "media-playback-stop-symbolic" :
				 "media-playback-start-symbolic");
	gtk_widget_set_sensitive(v->greeting_play,
				 (v->greeting_n > 0 || v->play) &&
				 !v->recording);
	gtk_widget_set_sensitive(v->greeting_clear,
				 v->greeting_n > 0 && !busy);
	char *g = v->recording ? g_strdup("Recording: speak, then Stop") :
		v->greeting_n ?
		g_strdup_printf("Your recording (%d s)",
				(v->greeting_n + DMC_VOICE_RATE / 2) /
				DMC_VOICE_RATE) :
		g_strdup("None: callers hear a beep");
	adw_action_row_set_subtitle(ADW_ACTION_ROW(v->greeting_row), g);
	g_free(g);
}

void voice_event(App *a, const dmc_event *ev)
{
	if (ev->type == DMC_EV_MESSAGE) {
		if (ev->ok)
			save_message(a, ev->text);
	} else if (ev->type == DMC_EV_VOICE) {
		if (!strcmp(ev->text, "b"))
			window_toast(a, "Busy");
		else if (!strcmp(ev->text, "l") &&
			 dmc_state(a->s) != DMC_VOICEMAIL)
			window_toast(a, "The other end hung up");
	}
}

/*
 *    the page
 */

static GtkWidget *button(const char *label, GCallback fn, App *a)
{
	GtkWidget *b = gtk_button_new_with_label(label);
	gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
	g_signal_connect(b, "clicked", fn, a);
	return b;
}

static GtkWidget *icon_button(const char *icon, const char *tip,
			      GCallback fn, App *a)
{
	GtkWidget *b = gtk_button_new_from_icon_name(icon);
	gtk_widget_set_tooltip_text(b, tip);
	gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
	gtk_widget_add_css_class(b, "flat");
	g_signal_connect(b, "clicked", fn, a);
	return b;
}

GtkWidget *voice_page_new(App *a)
{
	Voice *v = g_new0(Voice, 1);
	a->voice = v;
	v->message_rows = g_ptr_array_new();
	v->last_state = DMC_CLOSED;
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);
	gtk_widget_set_margin_top(box, 24);
	gtk_widget_set_margin_bottom(box, 24);
	gtk_widget_set_margin_start(box, 12);
	gtk_widget_set_margin_end(box, 12);

	/* the call */
	GtkWidget *cg = adw_preferences_group_new();
	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(cg), "Voice Call");
	adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(cg),
		"Calls through the modem's voice mode, with the computer's "
		"microphone and speakers");
	v->status = adw_action_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(v->status), "Phone");
	v->hangup_btn = button("Hang Up", G_CALLBACK(hangup), a);
	gtk_widget_add_css_class(v->hangup_btn, "destructive-action");
	v->pickup_btn = button("Pick Up", G_CALLBACK(pickup), a);
	gtk_widget_set_tooltip_text(v->pickup_btn,
				    "Talk to the caller yourself");
	v->listen_btn = gtk_toggle_button_new_with_label("Listen");
	gtk_widget_set_valign(v->listen_btn, GTK_ALIGN_CENTER);
	gtk_widget_set_tooltip_text(v->listen_btn,
				    "Hear the caller leave the message");
	g_signal_connect(v->listen_btn, "toggled", G_CALLBACK(listen_toggled), a);
	adw_action_row_add_suffix(ADW_ACTION_ROW(v->status), v->listen_btn);
	adw_action_row_add_suffix(ADW_ACTION_ROW(v->status), v->pickup_btn);
	adw_action_row_add_suffix(ADW_ACTION_ROW(v->status), v->hangup_btn);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(cg), v->status);

	v->number = adw_entry_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(v->number),
				      "Number to call");
	v->call_btn = button("Call", G_CALLBACK(call), a);
	gtk_widget_add_css_class(v->call_btn, "suggested-action");
	adw_entry_row_add_suffix(ADW_ENTRY_ROW(v->number), v->call_btn);
	g_signal_connect(v->number, "entry-activated", G_CALLBACK(call), a);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(cg), v->number);

	GtkWidget *ring = adw_action_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(ring), "A call ringing");
	v->answer_btn = button("Answer", G_CALLBACK(answer), a);
	v->message_btn = button("Take a Message", G_CALLBACK(take_message), a);
	adw_action_row_add_suffix(ADW_ACTION_ROW(ring), v->message_btn);
	adw_action_row_add_suffix(ADW_ACTION_ROW(ring), v->answer_btn);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(cg), ring);
	gtk_box_append(GTK_BOX(box), cg);

	/* the keypad: digits for the number, DTMF in a call */
	GtkWidget *pad = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(pad), 6);
	gtk_grid_set_column_spacing(GTK_GRID(pad), 6);
	gtk_grid_set_row_homogeneous(GTK_GRID(pad), TRUE);
	gtk_grid_set_column_homogeneous(GTK_GRID(pad), TRUE);
	static const char *const keys = "123456789*0#";
	for (int i = 0; i < 12; i++) {
		char k[2] = { keys[i], 0 };
		GtkWidget *b = gtk_button_new_with_label(k);
		gtk_widget_add_css_class(b, "title-3");
		gtk_widget_set_size_request(b, 72, 44);
		g_signal_connect(b, "clicked", G_CALLBACK(key), a);
		gtk_grid_attach(GTK_GRID(pad), b, i % 3, i / 3, 1, 1);
	}
	v->mute_btn = gtk_toggle_button_new_with_label("Mute");
	g_signal_connect(v->mute_btn, "toggled", G_CALLBACK(mute), a);
	gtk_grid_attach(GTK_GRID(pad), v->mute_btn, 0, 4, 3, 1);
	gtk_widget_set_halign(pad, GTK_ALIGN_CENTER);
	v->keypad = pad;
	gtk_box_append(GTK_BOX(box), pad);

	/* the answering machine */
	GtkWidget *mg = adw_preferences_group_new();
	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(mg),
					"Answering Machine");
	adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(mg),
		"Messages are kept on this computer only");
	v->vm_switch = adw_switch_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(v->vm_switch),
				      "Take messages");
	adw_action_row_set_subtitle(ADW_ACTION_ROW(v->vm_switch),
		"Answer calls nobody picks up, play the greeting and record");
	adw_switch_row_set_active(ADW_SWITCH_ROW(v->vm_switch), a->cfg.voicemail);
	g_signal_connect(v->vm_switch, "notify::active",
			 G_CALLBACK(voicemail_switched), a);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(mg), v->vm_switch);
	v->vm_rings = adw_spin_row_new_with_range(1, 15, 1);
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(v->vm_rings),
				      "Rings before it answers");
	adw_spin_row_set_value(ADW_SPIN_ROW(v->vm_rings), a->cfg.vm_rings);
	g_signal_connect(v->vm_rings, "notify::value",
			 G_CALLBACK(rings_changed), a);
	g_object_bind_property(v->vm_switch, "active", v->vm_rings,
			       "sensitive", G_BINDING_SYNC_CREATE);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(mg), v->vm_rings);
	v->greeting_row = adw_action_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(v->greeting_row),
				      "Greeting");
	v->greeting_play = icon_button("media-playback-start-symbolic", "Play",
				       G_CALLBACK(play_greeting), a);
	v->record_btn = button("Record", G_CALLBACK(record_greeting), a);
	GtkWidget *choose = icon_button("document-open-symbolic",
					"Use a WAV file",
					G_CALLBACK(choose_greeting), a);
	v->greeting_clear = icon_button("user-trash-symbolic",
					"No greeting: the beep alone",
					G_CALLBACK(clear_greeting), a);
	adw_action_row_add_suffix(ADW_ACTION_ROW(v->greeting_row), v->greeting_play);
	adw_action_row_add_suffix(ADW_ACTION_ROW(v->greeting_row), v->record_btn);
	adw_action_row_add_suffix(ADW_ACTION_ROW(v->greeting_row), choose);
	adw_action_row_add_suffix(ADW_ACTION_ROW(v->greeting_row), v->greeting_clear);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(mg), v->greeting_row);
	gtk_box_append(GTK_BOX(box), mg);

	/* the messages */
	v->messages = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
	adw_preferences_group_set_title(v->messages, "Messages");
	v->messages_empty = adw_action_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(v->messages_empty),
				      "No messages");
	gtk_widget_add_css_class(v->messages_empty, "dim-label");
	adw_preferences_group_add(v->messages, v->messages_empty);
	gtk_box_append(GTK_BOX(box), GTK_WIDGET(v->messages));

	char *d = data_dir();
	g_mkdir_with_parents(d, 0700);
	g_free(d);
	load_greeting(a);
	refresh_messages(a);
	voice_configure(a);

	GtkWidget *clamp = adw_clamp_new();
	adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 560);
	adw_clamp_set_child(ADW_CLAMP(clamp), box);
	GtkWidget *scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
				       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);
	return scroll;
}
