/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * dmodemctl.h - the part of D-Modem's desktop apps that is not drawing.
 *
 * The Windows, macOS and Linux apps are thin native shells over this
 * small C library, so that each of them is only views.  It does no I/O
 * of its own and starts no threads: each app opens the modem's DTE (a
 * pty, a TCP socket) in its own way, hands the bytes it reads to
 * dmc_feed(), writes what dmc_output() gives it, and calls dmc_poll()
 * from a timer.  That keeps it simple to call from C (GTK), C#
 * (P/Invoke) and Swift.
 *
 * Its parts:
 *  - dmc_session: the AT conversation with dmodemd: commands and their
 *    responses, the call's state from the result codes (RING, CONNECT,
 *    NO CARRIER), the escape to command mode and hanging up, and the
 *    stream for the app's terminal view.
 *  - dmc_model: the modem's settings, from AT!M, for a native settings
 *    page.
 *  - dmc_diag: the last call's statistics, from AT#UD (Microsoft's
 *    Unimodem diagnostics), as labelled rows.
 *  - dmc_vt: a small ANSI terminal screen, for the apps whose toolkit
 *    has no terminal widget (Windows and macOS): the AT console and the
 *    full-screen configurator (AT!CF).
 *  - dmc_daemon_*: starting dmodemd and reading what it prints.
 *  - dmc_net: a remote login's protocol (Telnet, rlogin) on a TCP
 *    connection the app makes, for the Remote page.
 *  - dmc_voice_*: voice calls on the session (V.253's voice mode: the
 *    call's audio both ways, 8 kHz), the answering machine that takes
 *    a message, and WAV files for greetings and messages.
 *
 * Strings are UTF-8.  A const char * the library returns stays valid
 * until the next call on the same object.
 */
#ifndef DMODEMCTL_H
#define DMODEMCTL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(DMC_BUILD_DLL)
#define DMC_API __declspec(dllexport)
#elif defined(_WIN32) && defined(DMC_USE_DLL)
#define DMC_API __declspec(dllimport)
#elif defined(__GNUC__)
#define DMC_API __attribute__((visibility("default")))
#else
#define DMC_API
#endif

/* the library's version: major * 10000 + minor * 100 + patch */
#define DMC_VERSION 10200
DMC_API int dmc_version(void);

/*
 * The session
 */

typedef struct dmc_session dmc_session;

/* (the enums have no tags: Swift takes their names as plain
   constants) */

/* the call's state, as the result codes tell it (dmc_state()) */
enum {
	DMC_CLOSED = 0,         /* no DTE open */
	DMC_IDLE,               /* command mode, on hook */
	DMC_DIALING,            /* ATD sent, no result yet */
	DMC_RINGING,            /* RING heard, not answered */
	DMC_ANSWERING,          /* ATA sent, no result yet */
	DMC_ONLINE,             /* CONNECT: data mode */
	DMC_ONLINE_COMMAND,     /* escaped (+++) during a call */
	DMC_HANGING_UP,         /* going back on hook */
	DMC_CONFIGURATOR,       /* AT!C, AT!CF or AT!CT runs */
	DMC_VOICE,              /* a voice call: its audio both ways */
	DMC_VOICEMAIL           /* the answering machine has the call */
};

/* what dmc_next_event() reports */
enum {
	DMC_EV_NONE = 0,
	/* a command of dmc_command()'s is done: tag, ok, text (its lines,
	   apart by '\n', without the echo and the final result), result
	   (the final result: OK, ERROR, CONNECT 33600..., or "timeout") */
	DMC_EV_RESPONSE,
	/* the state changed: state */
	DMC_EV_STATE,
	/* a result code nobody waited for (RING, NO CARRIER...): result */
	DMC_EV_RESULT,
	/* caller ID after a RING: text "NMBR=5551234" and the like */
	DMC_EV_CALLER,
	/* in a voice call, what the modem heard on the line: text its
	   <DLE> code ("5" a DTMF digit, "l" the far end hung up, "b" a busy
	   tone), result its name ("digit", "hangup", "busy") */
	DMC_EV_VOICE,
	/* the answering machine is done: ok 1 with a message
	   (dmc_voice_message()), 0 if the caller left none; text the
	   caller's number, if caller ID gave it */
	DMC_EV_MESSAGE
};

typedef struct dmc_event {
	int type;
	int tag;
	int ok;
	int state;
	const char *text;
	const char *result;
} dmc_event;

/* flags of dmc_command() */
enum {
	/* its echo and response kept out of the terminal stream (the app's
	   own queries: AT!M, AT#UD) */
	DMC_QUIET = 1
};

DMC_API dmc_session *dmc_session_new(void);
DMC_API void dmc_session_free(dmc_session *s);

/* the DTE opened (state IDLE) or closed (state CLOSED); either clears
   what was queued */
DMC_API void dmc_open(dmc_session *s, int64_t now_ms);
DMC_API void dmc_close(dmc_session *s);

/* bytes the modem sent */
DMC_API void dmc_feed(dmc_session *s, const char *data, int n);
/* bytes the user typed in the terminal view, sent as they are (the
   session reads the command lines in them, to follow ATD, ATA, ATH and
   AT!C typed by hand) */
DMC_API void dmc_type(dmc_session *s, const char *data, int n);
/* the time now: runs the timers (a command's timeout, the escape's
   guard times); returns the ms until it next needs to be called, -1 if
   only when something happens */
DMC_API int dmc_poll(dmc_session *s, int64_t now_ms);

/* bytes to write to the modem: up to max into buf, how many */
DMC_API int dmc_output(dmc_session *s, char *buf, int max);
/* bytes for the terminal view: up to max into buf, how many (what the
   modem sent, less the DMC_QUIET commands') */
DMC_API int dmc_terminal(dmc_session *s, char *buf, int max);
/* the next event into *ev: 1, or 0 for none */
DMC_API int dmc_next_event(dmc_session *s, dmc_event *ev);

/* an AT command (with or without the AT, no CR), queued behind any
   other; timeout_ms 0 for the usual (5 s, ATD and ATA two minutes);
   returns its tag (above 0) for the DMC_EV_RESPONSE, or -1 (closed,
   or online) */
DMC_API int dmc_command(dmc_session *s, const char *cmd, int flags,
			int timeout_ms);
/* dial a number (ATD), answer (ATA): a tag as dmc_command()'s */
DMC_API int dmc_dial(dmc_session *s, const char *number);
DMC_API int dmc_answer(dmc_session *s);
/* hang up whatever the state: online, the escape (+++, with its guard
   times) and ATH; dialling, a key to stop it; else ATH */
DMC_API void dmc_hangup(dmc_session *s);

DMC_API int dmc_state(const dmc_session *s);
/* the state's name for people: "Idle", "Dialling", "Online" */
DMC_API const char *dmc_state_name(int state);
/* the last CONNECT's text ("CONNECT 33600/ARQ/V34/LAPM/V42BIS"), its
   rate (0 none), the caller ID's number and name of the last RING */
DMC_API const char *dmc_connect_text(const dmc_session *s);
DMC_API int dmc_connect_rate(const dmc_session *s);
DMC_API const char *dmc_caller_number(const dmc_session *s);
DMC_API const char *dmc_caller_name(const dmc_session *s);

/*
 * Voice calls
 *
 * A voice call is the modem in voice mode (AT+FCLASS=8) with its audio
 * both ways (AT+VTR), G.711 mu-law on the DTE: the library sends ours
 * at the line's pace from dmc_poll() (call it every 20 ms or so in a
 * call: dmc_poll() says so) and gives the far end's to dmc_voice_read(),
 * both as 16-bit samples at 8000 a second.  dmc_hangup() ends a call,
 * and the far end's hanging up does too (DMC_EV_VOICE "l"); either way
 * the modem goes back to data mode.
 */

/* the sample rate of voice calls, messages and greetings */
#define DMC_VOICE_RATE 8000

/* dial a voice call (OK when the far end answers, BUSY, NO ANSWER);
   answer the ringing call as a voice call: a tag as dmc_command()'s,
   -1 if the modem is not idle (or ringing, to answer) */
DMC_API int dmc_voice_dial(dmc_session *s, const char *number);
DMC_API int dmc_voice_answer(dmc_session *s);

/* the far end's samples: up to max into pcm, how many (a second at
   most is kept: the oldest are dropped if they are not read) */
DMC_API int dmc_voice_read(dmc_session *s, int16_t *pcm, int max);
/* our samples (the microphone's), sent at the line's pace; silence
   when there are none, and the oldest dropped when more than a third
   of a second waits */
DMC_API void dmc_voice_write(dmc_session *s, const int16_t *pcm, int n);
/* a DTMF digit (0-9, *, #, A-D) played into the call, after any
   others still playing; 0, or -1 for no call or no digit */
DMC_API int dmc_voice_dtmf(dmc_session *s, char digit);

/*
 * The answering machine
 *
 * It answers the ringing call in voice mode, plays the greeting, a
 * beep, and records the caller's message: until they hang up, are
 * silent for silence_s seconds, or max_s seconds have gone.  Then it
 * hangs up, with DMC_EV_MESSAGE.  While it runs (DMC_VOICEMAIL) the
 * caller can be heard with dmc_voice_read() (screening), and
 * dmc_voice_pickup() makes it a voice call.
 */

/* the greeting (8 kHz samples, copied; none: the beep alone) and the
   message's limits (0: 120 s and 7 s) */
DMC_API void dmc_voicemail_set(dmc_session *s, const int16_t *greeting,
			       int n, int max_s, int silence_s);
/* answer the ringing call with the answering machine: a tag as
   dmc_voice_answer()'s */
DMC_API int dmc_voicemail_answer(dmc_session *s);
/* the answering machine answers by itself after rings RINGs (0 never,
   the default) */
DMC_API void dmc_voicemail_auto(dmc_session *s, int rings);
/* the call the answering machine has, taken as a voice call (its
   message dropped): 0, or -1 if it has none */
DMC_API int dmc_voice_pickup(dmc_session *s);
/* the last message's samples into *pcm (valid until the next message):
   how many */
DMC_API int dmc_voice_message(const dmc_session *s, const int16_t **pcm);
/* the RINGs of the call ringing now */
DMC_API int dmc_ring_count(const dmc_session *s);

/*
 * Sound files (greetings and messages)
 */

/* the 44-byte header of a WAV file of n 16-bit samples, 8 kHz, mono,
   into buf (44 bytes): 44 */
DMC_API int dmc_wav_header(char *buf, int n);
/* a sound file's samples at 8 kHz, mono, 16-bit: how many, *pcm then
   allocated (dmc_free() it); -1 if it is not one this reads. It reads
   WAV (PCM of 8, 16, 24 or 32 bits, 32 or 64-bit floats, A-law, mu-law,
   IMA and Microsoft ADPCM), AIFF and AIFF-C (PCM, floats, A-law,
   mu-law) and Sun/NeXT .au, of any rate and channels. (The apps read
   MP3, AAC, FLAC, Ogg and the like with their platforms' decoders.) */
DMC_API int dmc_audio_decode(const char *data, int n, int16_t **pcm);
/* (dmc_audio_decode's old name) */
DMC_API int dmc_wav_decode(const char *data, int n, int16_t **pcm);
DMC_API void dmc_free(void *p);

/*
 * The settings model (AT!M)
 */

typedef struct dmc_model dmc_model;

enum { DMC_PICK = 0, DMC_NUMBER, DMC_TEXT };

/* the model from AT!M's response text (dmc_event.text); NULL if it is
   not one */
DMC_API dmc_model *dmc_model_parse(const char *text);
DMC_API void dmc_model_free(dmc_model *m);

DMC_API int dmc_model_count(const dmc_model *m);
/* setting i's fields */
DMC_API const char *dmc_setting_key(const dmc_model *m, int i);
DMC_API int dmc_setting_kind(const dmc_model *m, int i);
DMC_API const char *dmc_setting_value(const dmc_model *m, int i);
DMC_API const char *dmc_setting_default(const dmc_model *m, int i);
DMC_API long dmc_setting_lo(const dmc_model *m, int i);
DMC_API long dmc_setting_hi(const dmc_model *m, int i);
DMC_API const char *dmc_setting_form(const dmc_model *m, int i);
DMC_API const char *dmc_setting_group(const dmc_model *m, int i);
DMC_API const char *dmc_setting_label(const dmc_model *m, int i);
DMC_API const char *dmc_setting_help(const dmc_model *m, int i);
DMC_API int dmc_setting_choices(const dmc_model *m, int i);
DMC_API const char *dmc_choice_value(const dmc_model *m, int i, int c);
DMC_API const char *dmc_choice_name(const dmc_model *m, int i, int c);
DMC_API const char *dmc_choice_label(const dmc_model *m, int i, int c);
/* the choice of setting i whose value is v, -1 none */
DMC_API int dmc_setting_choice_of(const dmc_model *m, int i,
				  const char *v);
/* the setting whose key is key, -1 none */
DMC_API int dmc_model_find(const dmc_model *m, const char *key);

/* the groups, in the order the settings list them */
DMC_API int dmc_model_groups(const dmc_model *m);
DMC_API const char *dmc_model_group(const dmc_model *m, int g);

/* "" if setting i takes value, else why not */
DMC_API const char *dmc_setting_check(const dmc_model *m, int i,
				      const char *value);
/* the command that sets setting i to value ("AT" and its form, the
   value in it), "" if it does not take it */
DMC_API const char *dmc_setting_command(const dmc_model *m, int i,
					const char *value);
/* setting i's value now value (after its command's OK) */
DMC_API void dmc_setting_set(dmc_model *m, int i, const char *value);

/*
 * The last call's statistics (AT#UD)
 */

typedef struct dmc_diag dmc_diag;

/* the statistics from AT#UD's response text; never NULL */
DMC_API dmc_diag *dmc_diag_parse(const char *text);
DMC_API void dmc_diag_free(dmc_diag *d);
/* how many rows, each its key (the specification's, hexadecimal), its
   label and its value for people ("33600 bit/s", "V.34") */
DMC_API int dmc_diag_count(const dmc_diag *d);
DMC_API int dmc_diag_key(const dmc_diag *d, int i);
DMC_API const char *dmc_diag_label(const dmc_diag *d, int i);
DMC_API const char *dmc_diag_value(const dmc_diag *d, int i);
/* the raw value of key, NULL if it was not reported */
DMC_API const char *dmc_diag_raw(const dmc_diag *d, int key);
/* there was a call to report (key 1 not "no previous call") */
DMC_API int dmc_diag_has_call(const dmc_diag *d);

/*
 * The terminal screen
 */

typedef struct dmc_vt dmc_vt;

/* a cell's attributes */
enum {
	DMC_BOLD = 1,
	DMC_UNDERLINE = 2,
	DMC_REVERSE = 4
};

/* the keys dmc_vt_key() encodes */
enum {
	DMC_KEY_UP = 1, DMC_KEY_DOWN, DMC_KEY_RIGHT, DMC_KEY_LEFT,
	DMC_KEY_HOME, DMC_KEY_END, DMC_KEY_PGUP, DMC_KEY_PGDN,
	DMC_KEY_INSERT, DMC_KEY_DELETE, DMC_KEY_BACKTAB,
	DMC_KEY_F1, DMC_KEY_F2, DMC_KEY_F3, DMC_KEY_F4, DMC_KEY_F5,
	DMC_KEY_F6, DMC_KEY_F7, DMC_KEY_F8, DMC_KEY_F9, DMC_KEY_F10,
	DMC_KEY_ENTER, DMC_KEY_BACKSPACE, DMC_KEY_TAB, DMC_KEY_ESCAPE
};

/* a screen of cols by rows, with scrollback lines kept above it */
DMC_API dmc_vt *dmc_vt_new(int cols, int rows, int scrollback);
DMC_API void dmc_vt_free(dmc_vt *t);
DMC_API void dmc_vt_resize(dmc_vt *t, int cols, int rows);
DMC_API void dmc_vt_reset(dmc_vt *t);
DMC_API int dmc_vt_cols(const dmc_vt *t);
DMC_API int dmc_vt_rows(const dmc_vt *t);
/* bytes from the modem (dmc_terminal()'s) */
DMC_API void dmc_vt_feed(dmc_vt *t, const char *data, int n);
/* the screen's answers to the modem's questions (where the cursor is:
   ESC [6n), to write to the modem as if typed */
DMC_API int dmc_vt_reply(dmc_vt *t, char *buf, int max);
/* how many lines of scrollback there are now */
DMC_API int dmc_vt_history(const dmc_vt *t);
/* a cell: row 0 the screen's top, -1 the newest scrollback line and so
   on; its character (a Unicode code point, 32 for none), colours (0 to
   15 the ANSI palette, -1 the default) and attributes */
DMC_API uint32_t dmc_vt_char(const dmc_vt *t, int row, int col);
DMC_API int dmc_vt_fg(const dmc_vt *t, int row, int col);
DMC_API int dmc_vt_bg(const dmc_vt *t, int row, int col);
DMC_API int dmc_vt_attr(const dmc_vt *t, int row, int col);
/* a row as UTF-8 text, trailing blanks dropped */
DMC_API const char *dmc_vt_text(dmc_vt *t, int row);
DMC_API int dmc_vt_cursor_row(const dmc_vt *t);
DMC_API int dmc_vt_cursor_col(const dmc_vt *t);
DMC_API int dmc_vt_cursor_visible(const dmc_vt *t);
/* the screen changed since the last call (and clears the mark) */
DMC_API int dmc_vt_changed(dmc_vt *t);
/* the bytes a key sends, into buf: how many */
DMC_API int dmc_vt_key(const dmc_vt *t, int key, char *buf, int max);
/* the RGB of palette colour c (0 to 15), as 0xRRGGBB */
DMC_API uint32_t dmc_vt_palette(int c);

/*
 * dmodemd
 */

/* a line dmodemd printed when it started: 1 and its DTE (a pty's
   path, "TCP 127.0.0.1:2323", "pipe \\.\pipe\dmodem") into buf if it is
   the line that says the modem is ready, else 0 */
DMC_API int dmc_daemon_ready(const char *line, char *buf, int max);
/* a line that is an error of dmodemd's ("error: ..."): 1 */
DMC_API int dmc_daemon_error(const char *line);

/*
 * Remote logins
 *
 * Telnet and rlogin on a TCP connection the app makes (SSH is the
 * system's ssh in a pty, so it needs nothing here).  Like the session it
 * does no I/O: hand it what the socket read, write what dmc_net_output()
 * gives, show what dmc_net_screen() gives.
 */

typedef struct dmc_net dmc_net;

enum { DMC_NET_TELNET = 1, DMC_NET_RLOGIN, DMC_NET_SSH };

/* the protocol's port (23, 513, 22) and its name ("Telnet") */
DMC_API int dmc_net_default_port(int proto);
DMC_API const char *dmc_net_name(int proto);
/* a login by Telnet or rlogin: user is rlogin's (Telnet's may be ""),
   term the terminal type the server is told, the screen's size */
DMC_API dmc_net *dmc_net_new(int proto, const char *user, const char *term,
			     int cols, int rows);
DMC_API void dmc_net_free(dmc_net *n);
/* the connection made: rlogin's handshake, Telnet's first offers */
DMC_API void dmc_net_connected(dmc_net *n);
/* bytes the socket read */
DMC_API void dmc_net_feed(dmc_net *n, const char *data, int len);
/* an urgent (out-of-band) byte the socket read: rlogin's control byte */
DMC_API void dmc_net_urgent(dmc_net *n, int byte);
/* the bytes for the terminal: how many */
DMC_API int dmc_net_screen(dmc_net *n, char *buf, int max);
/* bytes typed */
DMC_API void dmc_net_type(dmc_net *n, const char *data, int len);
/* the bytes to write to the socket: how many */
DMC_API int dmc_net_output(dmc_net *n, char *buf, int max);
/* the screen's new size, told to the server */
DMC_API void dmc_net_resize(dmc_net *n, int cols, int rows);

#ifdef __cplusplus
}
#endif

#endif
