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
 *  - dmc_vt: a terminal emulator, the app's ported to C: the AT
 *    console and the full-screen configurator (AT!CF), and the BBSes and
 *    hosts reached, as any of the terminals of the DEC VT line, xterm, a
 *    PC's ANSI-BBS, the Commodores', the Ataris', Viewdata, the Minitel
 *    and the Tektronix 4014.
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
#define DMC_VERSION 10300
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
 * The connection path
 *
 * The picture at the top of the Settings page of how a call is made, as
 * PCOMM's "Connection Overview" draws its link: four steps, left to right,
 * each with its role above its icon and what it is below, one line
 * joining them.  Idle, each step is as the line settings set it up (ready,
 * or something still to set); starting, calling and online, as it is now.
 * The words are here so that every app says the same; each draws them.
 */

typedef struct dmc_path dmc_path;

/* the line settings' kinds of line */
enum {
	DMC_LINE_SIP = 0,       /* dmodemd on a SIP account (or none) */
	DMC_LINE_USB,           /* dmodemd on a USB modem's phone line */
	DMC_LINE_CODEC,         /* dmodemd on a softmodem codec (ALSA) */
	DMC_LINE_DEVICE,        /* a modem that already runs, at a device */
	DMC_LINE_TCP,           /* dmodemd that already runs, over TCP */
	DMC_LINE_COM            /* a modem on a COM port */
};

/* a step's state: ready (set up, idle), something to set first, not in
   use, starting or connecting, in the call */
enum {
	DMC_STEP_READY = 0, DMC_STEP_SETUP, DMC_STEP_OFF, DMC_STEP_WORKING,
	DMC_STEP_ACTIVE
};

/* what a step is, for its icon */
enum {
	DMC_KIND_TERMINAL = 0, DMC_KIND_SOFTMODEM, DMC_KIND_MODEM,
	DMC_KIND_SIP, DMC_KIND_USB, DMC_KIND_CODEC, DMC_KIND_SERIAL,
	DMC_KIND_NETWORK, DMC_KIND_PHONE_NETWORK, DMC_KIND_SIP_ADDRESS,
	DMC_KIND_PHONE_LINE
};

DMC_API dmc_path *dmc_path_new(void);
DMC_API void dmc_path_free(dmc_path *p);
/* the line settings: the kind of line (DMC_LINE_*), where it is (--usb's
   device, the codec, the device's path, the TCP address, the COM port;
   NULL or "" for none), and the SIP account: none (anonymous), its user
   and server, taking calls (registering) */
DMC_API void dmc_path_set_line(dmc_path *p, int line, const char *where,
			       int sip_anonymous, const char *sip_user,
			       const char *sip_server, int listen);
/* now: dmodemd (or the device) started, the session's state
   (dmc_state(), DMC_CLOSED for none), the CONNECT line online */
DMC_API void dmc_path_set_live(dmc_path *p, int running, int state,
			       const char *connect);
/* the steps: how many, each one's kind, state (DMC_STEP_*), role ("Modem",
   the label above it), title ("D-Modem") and what it is ("Registers as
   alice@example.com") */
DMC_API int dmc_path_steps(const dmc_path *p);
DMC_API int dmc_path_kind(const dmc_path *p, int i);
DMC_API int dmc_path_state(const dmc_path *p, int i);
DMC_API const char *dmc_path_role(const dmc_path *p, int i);
DMC_API const char *dmc_path_title(const dmc_path *p, int i);
DMC_API const char *dmc_path_detail(const dmc_path *p, int i);
/* the line from step i to the next: the lesser of the two's states
   (off, then to set, working, ready, active) */
DMC_API int dmc_path_link(const dmc_path *p, int i);
/* the whole in a line: what needs setting, or what it's doing, or that
   it's ready */
DMC_API const char *dmc_path_headline(const dmc_path *p);
/* some step is still to set */
DMC_API int dmc_path_needs_setup(const dmc_path *p);

/*
 * The terminal screen
 *
 * A terminal emulator: the app's (D-Modem/app) ported to C, as one of
 * many kinds of terminal (a profile): DEC's VT52 to VT525, xterm, a PC's
 * ANSI-BBS (with Avatar, RIPscrip and Amiga ANSI), the Commodores'
 * PETSCII, the Atari's ATASCII and ST, Viewdata, the Minitel and the
 * Tektronix 4014.  Bytes from the remote go to dmc_vt_feed(); what it
 * answers (dmc_vt_reply()) and what keys send (dmc_vt_key_mods(),
 * dmc_vt_char_bytes()) go back to it.  A view draws each cell from
 * dmc_vt_cell(): its character, its colours resolved, its attributes, and
 * where the profile has its machine's font, the glyph's bitmap.
 */

typedef struct dmc_vt dmc_vt;

/* a cell's attributes (dmc_vt_attr) */
enum {
	DMC_BOLD = 1,
	DMC_UNDERLINE = 2,
	DMC_REVERSE = 4
};

/* the keys dmc_vt_key() encodes (the values stay as they are: new ones
   are added at the end) */
enum {
	DMC_KEY_UP = 1, DMC_KEY_DOWN, DMC_KEY_RIGHT, DMC_KEY_LEFT,
	DMC_KEY_HOME, DMC_KEY_END, DMC_KEY_PGUP, DMC_KEY_PGDN,
	DMC_KEY_INSERT, DMC_KEY_DELETE, DMC_KEY_BACKTAB,
	DMC_KEY_F1, DMC_KEY_F2, DMC_KEY_F3, DMC_KEY_F4, DMC_KEY_F5,
	DMC_KEY_F6, DMC_KEY_F7, DMC_KEY_F8, DMC_KEY_F9, DMC_KEY_F10,
	DMC_KEY_ENTER, DMC_KEY_BACKSPACE, DMC_KEY_TAB, DMC_KEY_ESCAPE,
	DMC_KEY_F11, DMC_KEY_F12,
	/* an LK201's F13-F20 (F15 its Help, F16 its Do), Find and Select */
	DMC_KEY_F13, DMC_KEY_F14, DMC_KEY_F15, DMC_KEY_F16, DMC_KEY_F17,
	DMC_KEY_F18, DMC_KEY_F19, DMC_KEY_F20, DMC_KEY_FIND, DMC_KEY_SELECT,
	/* the DEC keypad */
	DMC_KEY_PF1, DMC_KEY_PF2, DMC_KEY_PF3, DMC_KEY_PF4,
	DMC_KEY_KP_0, DMC_KEY_KP_1, DMC_KEY_KP_2, DMC_KEY_KP_3, DMC_KEY_KP_4,
	DMC_KEY_KP_5, DMC_KEY_KP_6, DMC_KEY_KP_7, DMC_KEY_KP_8, DMC_KEY_KP_9,
	DMC_KEY_KP_MINUS, DMC_KEY_KP_COMMA, DMC_KEY_KP_PERIOD, DMC_KEY_KP_ENTER
};

/* the modifiers held with a key */
enum { DMC_MOD_SHIFT = 1, DMC_MOD_ALT = 2, DMC_MOD_CTRL = 4 };

/* the character sets the remote may speak (a profile can have its own) */
enum { DMC_CHARSET_UTF8, DMC_CHARSET_CP437, DMC_CHARSET_LATIN1 };

/* a cell as a view draws it (dmc_vt_cell) */
enum {
	DMC_CELL_BOLD = 1,              /* a heavy face (bold not shown as bright) */
	DMC_CELL_ITALIC = 2,
	DMC_CELL_UNDERLINE = 4,
	DMC_CELL_DOUBLE_UNDERLINE = 8,
	DMC_CELL_CURLY_UNDERLINE = 16,
	DMC_CELL_STRIKE = 32,
	DMC_CELL_OVERLINE = 64,
	DMC_CELL_BLINK = 128,           /* drawn blank in the blink's off phase */
	DMC_CELL_INVISIBLE = 256,       /* not drawn (its background is) */
	DMC_CELL_WIDE = 512,            /* two cells wide: the next is its tail */
	DMC_CELL_WIDE_TAIL = 1024,      /* the right half of the one before */
	DMC_CELL_IMAGE = 2048,          /* a picture's tile (dmc_vt_cell_image) */
	DMC_CELL_GLYPH = 4096,          /* drawn from a bitmap (dmc_vt_glyph_row) */
	DMC_CELL_COMBINING = 8192,      /* has combining marks (dmc_vt_cell_text) */
	DMC_CELL_LINK = 16384           /* a hyperlink (dmc_vt_cell_link) */
};

typedef struct dmc_cell {
	uint32_t ch;            /* a code point (32 blank, 0 a wide tail) */
	uint32_t fg, bg;        /* 0xRRGGBB, resolved (bold, inverse ...) */
	int flags;              /* DMC_CELL_* */
} dmc_cell;

/* where a cell's tile is in a picture */
typedef struct dmc_image {
	uint32_t id;            /* the same while the picture is */
	int width, height;      /* the picture's pixels */
	int cell_width, cell_height; /* its tiles' size */
	int tx, ty;             /* this cell's tile */
} dmc_image;

/* the line sizes (DECDWL, DECDHL) */
enum {
	DMC_LINE_SINGLE, DMC_LINE_DOUBLE_WIDTH, DMC_LINE_DOUBLE_TOP,
	DMC_LINE_DOUBLE_BOTTOM
};

/* a screen of cols by rows, with scrollback lines kept above it; an
   xterm until dmc_vt_set_profile() */
DMC_API dmc_vt *dmc_vt_new(int cols, int rows, int scrollback);
DMC_API void dmc_vt_free(dmc_vt *t);
DMC_API void dmc_vt_resize(dmc_vt *t, int cols, int rows);
/* everything as at the start, the scrollback cleared */
DMC_API void dmc_vt_reset(dmc_vt *t);
DMC_API int dmc_vt_cols(const dmc_vt *t);
DMC_API int dmc_vt_rows(const dmc_vt *t);
/* bytes from the remote (dmc_terminal()'s) */
DMC_API void dmc_vt_feed(dmc_vt *t, const char *data, int n);
/* the screen's answers to the remote's questions (where the cursor is,
   what terminal it is), to write to it as if typed */
DMC_API int dmc_vt_reply(dmc_vt *t, char *buf, int max);
/* how many lines of scrollback there are now */
DMC_API int dmc_vt_history(const dmc_vt *t);
/* a cell: row 0 the screen's top, -1 the newest scrollback line and so
   on; its character (a Unicode code point, 32 for none), colours (0 to
   255 the palette, -1 the default or a 24-bit one) and attributes */
DMC_API uint32_t dmc_vt_char(const dmc_vt *t, int row, int col);
DMC_API int dmc_vt_fg(const dmc_vt *t, int row, int col);
DMC_API int dmc_vt_bg(const dmc_vt *t, int row, int col);
DMC_API int dmc_vt_attr(const dmc_vt *t, int row, int col);
/* a cell as it is drawn: 0 off the screen (c then blank) */
DMC_API int dmc_vt_cell(const dmc_vt *t, int row, int col, dmc_cell *c);
/* a cell's character with its combining marks, as UTF-8 */
DMC_API const char *dmc_vt_cell_text(dmc_vt *t, int row, int col);
/* a cell's hyperlink (OSC 8), NULL for none */
DMC_API const char *dmc_vt_cell_link(const dmc_vt *t, int row, int col);
/* the picture a cell shows a tile of: its pixels (0xAARRGGBB, width *
   height, not premultiplied), NULL for none */
DMC_API const uint32_t *dmc_vt_cell_image(const dmc_vt *t, int row, int col,
					  dmc_image *im);
/* a row's size (DMC_LINE_*): its cells twice as wide, and for double
   height the top or bottom half of them */
DMC_API int dmc_vt_line_size(const dmc_vt *t, int row);
/* a row as UTF-8 text, trailing blanks dropped */
DMC_API const char *dmc_vt_text(dmc_vt *t, int row);
/* the text from (row, col) to (row2, col2), inclusive: wrapped lines
   joined, the others ended with a newline */
DMC_API const char *dmc_vt_text_range(dmc_vt *t, int row, int col, int row2,
				      int col2);
DMC_API int dmc_vt_cursor_row(const dmc_vt *t);
DMC_API int dmc_vt_cursor_col(const dmc_vt *t);
DMC_API int dmc_vt_cursor_visible(const dmc_vt *t);
/* DECSCUSR: 0 the view's; 1, 2 block; 3, 4 underline; 5, 6 bar */
DMC_API int dmc_vt_cursor_style(const dmc_vt *t);
/* the screen changed since the last call (and clears the mark) */
DMC_API int dmc_vt_changed(dmc_vt *t);

/* the bytes a key sends, into buf: how many (dmc_vt_key: no modifiers) */
DMC_API int dmc_vt_key(const dmc_vt *t, int key, char *buf, int max);
DMC_API int dmc_vt_key_mods(dmc_vt *t, int key, int mods, char *buf, int max);
/* the bytes a character typed (or pasted) sends, as this terminal sends
   it (a Commodore's PETSCII, Ctrl's control codes, Alt as ESC) */
DMC_API int dmc_vt_char_bytes(dmc_vt *t, uint32_t cp, int mods, char *buf,
			      int max);
/* what Enter sends (0 CR, 1 CR LF, 2 LF) and Backspace (0x08 or 0x7f) */
DMC_API void dmc_vt_set_enter(dmc_vt *t, int mode);
DMC_API void dmc_vt_set_backspace(dmc_vt *t, int code);
/* a mouse event, when the remote asked for them: button 0-2, 64 and 65
   the wheel; the bytes to send (0: none) */
DMC_API int dmc_vt_mouse_wanted(const dmc_vt *t);
DMC_API int dmc_vt_mouse(dmc_vt *t, int button, int col, int row, int press,
			 int motion, int mods, char *buf, int max);
/* a click at (fx, fy), fractions of the screen from its top left: what
   it sends (a RIPscrip button's), 0 for nothing */
DMC_API int dmc_vt_tap(dmc_vt *t, double fx, double fy, char *buf, int max);
/* the remote asked for pastes bracketed (ESC [200~ ... ESC [201~) */
DMC_API int dmc_vt_bracketed_paste(const dmc_vt *t);
/* the focus's change, when the remote asked to be told */
DMC_API int dmc_vt_focus(dmc_vt *t, int focused, char *buf, int max);

/* the kinds of terminal, in the pickers' order: their ids (kept in
   settings), names, the TERM for SSH and the TERMINAL-TYPE for telnet */
DMC_API int dmc_vt_profile_count(void);
DMC_API const char *dmc_vt_profile_id(int i);
DMC_API const char *dmc_vt_profile_label(int i);
DMC_API const char *dmc_vt_profile_term(const char *id);
DMC_API const char *dmc_vt_profile_telnet(const char *id);
/* the kind of terminal it is (1), or 0 for an id not known */
DMC_API int dmc_vt_set_profile(dmc_vt *t, const char *id);
DMC_API const char *dmc_vt_get_profile(const dmc_vt *t);
/* the user's character set, for the profiles that haven't their own */
DMC_API void dmc_vt_set_charset(dmc_vt *t, int charset);
/* the columns and rows the view should fit (DECCOLM's 80 or 132, a
   Commodore's 40x25), 0 for as many as fit */
DMC_API int dmc_vt_requested_cols(const dmc_vt *t);
DMC_API int dmc_vt_requested_rows(const dmc_vt *t);
/* a cell's size in pixels as the view draws it (sixel images are
   measured in it, and it is reported to the remote) */
DMC_API void dmc_vt_set_cell_pixels(dmc_vt *t, int w, int h);
/* the theme: the default foreground, background and cursor (0xRRGGBB) and
   the 16 colours (NULL: kept); bold shown as the bright colours */
DMC_API void dmc_vt_set_theme(dmc_vt *t, uint32_t fg, uint32_t bg,
			      uint32_t cursor, const uint32_t *palette16);
DMC_API void dmc_vt_set_bold_bright(dmc_vt *t, int on);
/* the screen's background, foreground and cursor as shown now (a
   machine's own, or the remote's, or the theme's) */
DMC_API uint32_t dmc_vt_background(const dmc_vt *t);
DMC_API uint32_t dmc_vt_foreground(const dmc_vt *t);
DMC_API uint32_t dmc_vt_cursor_color(const dmc_vt *t);
/* the bitmap font the profile's machine had (its cell's size: 1), 0 for
   the view's own font */
DMC_API int dmc_vt_glyph_cell(const dmc_vt *t, int *w, int *h);
/* whether cp is drawn from a bitmap (the machine's font, a soft
   character), and its size */
DMC_API int dmc_vt_glyph(const dmc_vt *t, uint32_t cp, int *w, int *h);
/* row (0 top) of cp's bitmap, its leftmost pixel bit w - 1 */
DMC_API uint32_t dmc_vt_glyph_row(const dmc_vt *t, uint32_t cp, int row);
/* what the status line below the screen shows ("" for none: no line) */
DMC_API const char *dmc_vt_status_line(dmc_vt *t);
/* the window's title as the remote set it, and whether it changed */
DMC_API const char *dmc_vt_title(dmc_vt *t);
DMC_API int dmc_vt_title_changed(const dmc_vt *t);
/* how many times the bell rang since the last call */
DMC_API int dmc_vt_bells(dmc_vt *t);
/* ANSI music: the next note to play (1), its pitch in hundredths of a
   hertz (0 a rest), how long it sounds and the silence after it */
DMC_API int dmc_vt_music(dmc_vt *t, uint32_t *hz100, int *ms, int *rest_ms);
/* what the remote sent to the printer (UTF-8), taken */
DMC_API int dmc_vt_printed(dmc_vt *t, char *buf, int max);
/* the line speed the remote asked its text be shown at (CSI * r), 0 */
DMC_API int dmc_vt_emulated_bps(const dmc_vt *t);
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
