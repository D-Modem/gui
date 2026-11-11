/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * app.h - the Linux app (GTK 4, libadwaita, VTE): what its parts share
 */
#ifndef DMODEM_GUI_APP_H
#define DMODEM_GUI_APP_H

#include <adwaita.h>
#include <vte/vte.h>

#include "dmodemctl.h"

#define APP_ID "io.github.d_modem.DModem"

/* the line settings, kept in ~/.config/dmodem-gui/settings.ini */
typedef struct Config {
	/* "sip", "usb", "alsa", or "device" (a modem that already runs) */
	char *line;
	char *sip_user, *sip_password, *sip_server;
	gboolean listen, t38, v152, v150;
	char *srtp;             /* "", "no" or "require" */
	char *country;          /* dmodemd -c's, "" for its default */
	char *usb, *alsa;       /* --usb's (auto), -a's (hw:1) */
	char *device;           /* the modem's device, "device" line */
	char *dmodemd;          /* its path, "" to look for it */
	char *init;             /* commands sent when the modem opens */
	int debug;              /* dmodemd -d */
	/* the answering machine ([voice]): on, after how many RINGs, a
	   message's longest (s) */
	gboolean voicemail;
	int vm_rings, vm_max;
} Config;

typedef struct App App;

/* a command's response */
typedef void (*ResponseFn)(App *a, const dmc_event *ev, gpointer data);

struct App {
	AdwApplication *gapp;
	GtkWindow *window;
	AdwToastOverlay *toasts;
	Config cfg;

	/* the modem */
	dmc_session *s;
	GSubprocess *proc;
	GCancellable *cancel;
	char *device;           /* the DTE's path */
	int fd;
	guint fd_watch, tick;
	GByteArray *pending;    /* written when the DTE takes it */
	GHashTable *waiting;    /* tag -> struct Waiting */
	int last_state;

	/* the pages' widgets */
	AdwViewStack *stack;
	GtkWidget *status_icon, *status_title, *status_detail;
	GtkWidget *number, *dial_btn, *answer_btn, *hangup_btn;
	GtkWidget *run_row, *run_btn, *device_row, *copy_btn;
	VteTerminal *term;
	GtkWidget *term_buttons;
	AdwPreferencesPage *settings_page;
	GPtrArray *settings_groups;
	GPtrArray *setting_rows;        /* by the setting's index */
	int settings_gen;               /* a new model: older answers ignored */
	GtkWidget *settings_status, *init_row;
	dmc_model *model;
	gboolean settings_quiet;   /* (rows set by the app, not the user) */
	AdwPreferencesGroup *stats_group;
	GPtrArray *stats_rows;
	GtkWidget *stats_report, *stats_empty;
	GtkTextBuffer *log;

	/* the Phone page (voice.c) */
	struct Voice *voice;
};

/* config.c */
void config_load(Config *c);
void config_save(const Config *c);
void config_free(Config *c);
/* "SIP: alice@sip.example.com" and the like */
char *config_summary(const Config *c);
/* the line settings dialog */
void prefs_show(App *a);

/* modem.c */
void modem_start(App *a);
void modem_stop(App *a);
gboolean modem_running(App *a);
/* bytes typed in the terminal */
void modem_type(App *a, const char *data, gsize n);
/* a command; fn (may be NULL) gets its response */
int modem_command(App *a, const char *cmd, int flags, ResponseFn fn,
		  gpointer data);
/* fn (may be NULL) gets the response of the session's command tag
   (dmc_voice_dial()'s and the like) */
void modem_wait(App *a, int tag, ResponseFn fn, gpointer data);
/* the session's output, terminal bytes and events handled now */
void modem_pump(App *a);
void app_log(App *a, const char *line);

/* window.c */
void window_build(App *a);
void window_state(App *a);
void window_toast(App *a, const char *text);
/* the modem's events (the session's DMC_EV_*) */
void window_event(App *a, const dmc_event *ev);
void window_opened(App *a);
void window_closed(App *a);

/* settings.c */
GtkWidget *settings_page_new(App *a);
void settings_load(App *a);
void settings_clear(App *a);
void settings_sensitive(App *a, gboolean on);

/* voice.c: the Phone page, voice calls' audio, the answering machine
   and its messages */
GtkWidget *voice_page_new(App *a);
void voice_state(App *a);
void voice_event(App *a, const dmc_event *ev);
/* the answering machine's settings to the session */
void voice_configure(App *a);

/* audio.c: the sound card, 8 kHz mono (PulseAudio, or PipeWire's) */
/* the speaker's next samples: how many of n there were (the rest
   silence); the microphone's */
typedef int (*AudioPull)(App *a, int16_t *pcm, int n);
typedef void (*AudioPush)(App *a, const int16_t *pcm, int n);
/* sound from pull and to push (either NULL for none): FALSE if there
   is no sound here */
gboolean audio_start(App *a, AudioPull pull, AudioPush push);
void audio_stop(App *a);
gboolean audio_available(void);

/* stats.c */
GtkWidget *stats_page_new(App *a);
void stats_refresh(App *a);

#endif
