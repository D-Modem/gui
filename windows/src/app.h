/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * app.h - the Windows app: plain Win32 in C++98, ANSI, the common
 *         controls of Windows 95, so the one .exe runs on Windows 95,
 *         98 and ME and on NT 4 through Windows 11.  Built with
 *         clang-retro (cmake/clang-retro.cmake), or MinGW or MSVC.
 *
 * The modem is reached one of three ways (Link):
 *   - dmodemd started by the app on this PC (Windows 7 and later, as
 *     dmodemd needs), its DTE a TCP socket on 127.0.0.1;
 *   - a TCP address: dmodemd as a service, or on another computer;
 *   - a COM port: a modem's serial port, a pico-serial, or dmodemd's
 *     --com through a com0com pair.
 */
#ifndef DMODEM_WIN_APP_H
#define DMODEM_WIN_APP_H

#ifndef WINVER
#define WINVER 0x0400
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0400
#endif
#ifndef _WIN32_IE
#define _WIN32_IE 0x0300
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock.h>
#include <commctrl.h>

#include "dmodemctl.h"

/* (picolibc, the C library of the clang-retro build, has the C99 and
   POSIX names and not the names of Microsoft's C library) */
#include <stdio.h>
#include <string.h>
#ifdef __PICOLIBC__
#include <strings.h>
#define _snprintf snprintf
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#endif

/* the separator of paths that libssh and the C library see: Windows', or
   POSIX on Linux and macOS (winport/); and the name of the folder that
   holds .ssh */
#ifdef DMODEM_WINPORT
#define DIR_SEP "/"
#define PROFILE_NAME "home folder"
#else
#define DIR_SEP "\\"
#define PROFILE_NAME "profile"
#endif

/* (older headers, Open Watcom's among them, without the 64-bit names) */
#ifndef GWLP_USERDATA
#define GWLP_USERDATA GWL_USERDATA
#define GetWindowLongPtr GetWindowLong
#define SetWindowLongPtr SetWindowLong
#define GWLP_WNDPROC GWL_WNDPROC
#endif

/* the line settings, kept in dmodem.ini */
enum LineKind { LINE_DMODEMD = 0, LINE_TCP, LINE_COM };

struct Config {
	int line;                       /* LineKind */
	char sip_user[128], sip_password[128], sip_server[256];
	/* SIP with no account (dmodemd's SIP_LOGIN=anonymous): nothing
	   registered, SIP addresses (sip:user@host, user@host) dialled as
	   they are, from "Anonymous" */
	int sip_anonymous;
	int listen, t38, v152, v150;
	char country[64];
	char usb[64];                   /* --usb, "" for a SIP line */
	char dmodemd[MAX_PATH];         /* dmodemd.exe, "" next to the app */
	char tcp[256];                  /* host:port */
	char com[32];                   /* COM1 */
	char init[256];                 /* commands when the modem opens */
	/* the answering machine ([voice]): on, after how many RINGs, a
	   message's longest (s) */
	int voicemail, vm_rings, vm_max;
};

/* where the app keeps its files: %APPDATA%\D-Modem, or beside the
   app */
void app_data_dir(char *out, int n);
void config_load(Config *c);
void config_save(const Config *c);
/* "SIP: alice@sip.example.com" and the like, into out */
void config_summary(const Config *c, char *out, int n);
/* the SIP line has what dmodemd needs: an account, or none (anonymous) */
bool config_sip_ready(const Config *c);
/* the Line Settings dialog: true if changed */
bool line_dialog(HWND owner, Config *c);

/* a command's response */
typedef void (*ResponseFn)(const dmc_event *ev, void *data);

/* the app's state */
struct App {
	HINSTANCE inst;
	HWND main, tabs;
	HWND pages[7];
	HFONT font, bold_font, big_font, mono_font;
	Config cfg;
	dmc_session *s;
	int last_state;
	char device[256];               /* what the modem is at, for people */
};

extern App app;

/* link.cpp: the modem's connection */
void link_start();
void link_stop();
bool link_running();
bool link_open();                   /* the DTE is open */
/* a break on the line (a COM port's only): false when none can be sent */
bool link_break();
/* the session's output written, input read, events handled */
void link_pump();
/* socket messages (WM_APP_SOCKET) */
void link_socket(WPARAM w, LPARAM l);
void link_type(const char *data, int n);
/* bytes to the far end as they are (a file transfer's) */
void link_send(const char *data, int n);
/* how many more bytes link_send can take now */
int link_room();
int link_command(const char *cmd, int flags, ResponseFn fn, void *data);
/* fn gets the response of the session's command tag (dmc_voice_dial()'s
   and the like) */
void link_wait(int tag, ResponseFn fn, void *data);

#define WM_APP_SOCKET (WM_APP + 1)
#define TIMER_PUMP 1
#define TIMER_VOICE 2
#define WM_APP_REMOTE (WM_APP + 2)
#define TIMER_REMOTE 3
/* an event of the Remote page's SSH connection, from its thread */
#define WM_APP_SSH (WM_APP + 3)
/* a file transfer has bytes for its line, or ended (wParam 0); the
   remote's rz waits for files (wParam 1, lParam its XferLine) */
#define WM_APP_XFER (WM_APP + 4)

/* main.cpp */
void app_log(const char *line);
void app_message(const char *text);     /* the status bar's */
/* the modem's events, the state */
void app_event(const dmc_event *ev);
void app_state();
void app_opened();
void app_closed();

/* pages.cpp: a page (a plain child window whose controls' commands go
   to fn), and the Modem, Call Stats and Log pages */
typedef void (*CommandFn)(HWND page, int id, int code);
HWND page_new(HWND parent, CommandFn fn);
HWND modem_page_new(HWND parent);
void modem_page_state();
void modem_page_layout(HWND page, int w, int h);
HWND stats_page_new(HWND parent);
void stats_page_layout(HWND page, int w, int h);
void stats_refresh();
HWND log_page_new(HWND parent);
void log_page_layout(HWND page, int w, int h);
void log_append(const char *line);

/* term.cpp: the Terminal page */
HWND term_page_new(HWND parent);
void term_page_layout(HWND page, int w, int h);
void term_feed(const char *data, int n);
void term_state();

/* termview.cpp: a terminal control of its own screen (dmc_vt), whose
   keys go to fn */
typedef void (*TermTypedFn)(const char *data, int n, void *ctx);
HWND termview_new(HWND parent, int id, TermTypedFn fn, void *ctx);
/* bytes from the far end */
void termview_feed(HWND w, const char *data, int n);
void termview_reset(HWND w);
void termview_size(HWND w, int *cols, int *rows);
/* what Backspace sends (DEL unless set) */
void termview_set_backspace(HWND w, char b);
/* Terminal Settings: Backspace sends Ctrl+H (0), Del (1) or Ctrl+H,
   Space, Ctrl+H (2) (-1: as it is); the function, arrow and Ctrl keys as
   Windows keys; the backscroll's lines; the font (none: the app's) and
   its size in points; the cursor (0 block, 1 underline) and its blink */
struct TermOptions {
	int backspace;
	bool windows_keys;
	int scrollback;
	char font[32];
	int font_size;
	int cursor;
	bool cursor_blink;
};
void termview_set_options(HWND w, const TermOptions *o);
/* ASCII Setup: the received data's DMC_VT_ASCII_* flags, what Enter sends
   (0 CR, 1 CR LF), and the local echo */
void termview_set_ascii(HWND w, int flags, int enter, bool echo);
/* the selection (drag with the left button, double-click a word): to
   the clipboard; the clipboard's text typed; all the lines selected */
bool termview_has_selection(HWND w);
bool termview_copy(HWND w);
void termview_paste(HWND w);
void termview_select_all(HWND w);
/* the selection's text, or the screen's (UTF-8, malloc'ed) */
char *termview_text(HWND w, bool selection);
/* what the remote sends to its printer (UTF-8) goes to fn */
typedef void (*TermPrintedFn)(const char *data, int n, void *ctx);
void termview_set_printer(HWND w, TermPrintedFn fn, void *ctx);
/* the kind of terminal it is (dmc_vt_set_profile()'s id) */
void termview_set_profile(HWND w, const char *id);
const char *termview_profile(HWND w);
/* a combo box of the kinds of terminal, id chosen; the one chosen */
void term_types_fill(HWND combo, const char *id);
const char *term_types_chosen(HWND combo);

/* xfer.cpp: file transfers on a terminal's line (the Terminal page's
   modem, the Remote page's login) */
struct XferLine {
	const char *what;               /* "the modem", for messages */
	HWND *view;                     /* its terminal control */
	void (*write)(const char *data, int n);  /* to the far end, as they are */
	int (*room)();                  /* how much write can take now */
	bool (*open)();                 /* a far end to send to */
};
extern XferLine term_line, remote_line;
/* what came on the line: a transfer's (true), else the terminal's; the
   remote's own starts (its sz, kermit -s, rz) noticed */
bool xfer_take(XferLine *l, const char *data, int n);
/* the transfer's bytes to its line, its dialog brought up to date */
void xfer_pump();
/* WM_APP_XFER */
void xfer_message(WPARAM w, LPARAM l);
/* the Send File and Receive File dialogs (protocol -1: the last one) */
void xfer_send_dialog(XferLine *l, int protocol);
void xfer_receive_dialog(XferLine *l);
void xfer_cancel();
bool xfer_running();
/* a transfer runs on l: what's typed doesn't go */
bool xfer_busy(XferLine *l);
bool xfer_auto();
void xfer_set_auto(bool on);
/* the transfer's dialog's keys (IsDialogMessage) */
bool xfer_dialog_key(MSG *m);

/* termset.cpp: Terminal Settings, for the modem's terminal or the
   Remote page's ([terminal], [remote_terminal] in dmodem.ini) */
void termset_apply(HWND view, bool remote);
void termset_dialog(HWND parent, bool remote);
/* the Telnet terminal ID set (empty: the terminal type's) */
void termset_telnet_id(char *out, int n);
bool termset_exit_on_disconnect();

/* ascii.cpp: ASCII Setup ([ascii] in dmodem.ini), for both terminals */
struct AsciiSetup {
	bool send_lf;           /* Enter sends CR LF */
	bool echo;              /* what is typed shown here too */
	int line_delay;         /* a text file sent: ms after each line */
	int char_delay;         /* and after each character */
	bool add_lf;            /* an LF added after each CR received */
	bool seven_bit;         /* the data received as 7-bit ASCII */
	bool wrap;              /* lines wrap at the right margin */
};
void ascii_load(AsciiSetup *a);
/* the settings to a terminal control */
void ascii_apply(HWND view);
void ascii_dialog(HWND parent);

/* capture.cpp: Capture Text, Capture to Printer and Print, for a line */
enum { CAPTURE_OFF, CAPTURE_ON, CAPTURE_PAUSED };
/* what came on the line for its terminal (not a transfer's bytes) */
void capture_feed(XferLine *l, const char *data, int n);
/* the remote's printing (a TermPrintedFn; ctx its XferLine) */
void capture_printed(const char *data, int n, void *ctx);
/* the remote's printing printed once it stops (the main window's timer) */
void capture_tick();
void capture_text_start(XferLine *l);
void capture_text_stop(XferLine *l);
void capture_text_pause(XferLine *l, bool pause);
int capture_text_state(XferLine *l);
void capture_printer(XferLine *l, bool on);
void capture_printer_pause(XferLine *l, bool pause);
int capture_printer_state(XferLine *l);
/* the screen or the selection to a printer (the print dialog) */
void capture_print(XferLine *l);
void capture_close_all();

/* remote.cpp: the Remote page, a login by SSH, Telnet or rlogin */
HWND remote_page_new(HWND parent);
void remote_page_layout(HWND page, int w, int h);
/* its socket's messages (WM_APP_REMOTE) and its timer (TIMER_REMOTE) */
void remote_socket(WPARAM w, LPARAM l);
void remote_tick();
/* an SSH event (WM_APP_SSH's lParam) */
void remote_ssh(LPARAM l);
/* the focus to its first control */
bool remote_focus();
void remote_stop();
/* a break to the server (Telnet's): false when none can be sent */
bool remote_break();

/* settings.cpp: the Settings page */
HWND settings_page_new(HWND parent);
/* pathview.cpp: the connection path at the top of the Settings page, as
   the line settings and the modem are now; its height */
HWND pathview_new(HWND parent);
void pathview_update();
int pathview_height();
void settings_page_layout(HWND page, int w, int h);
void settings_load();
void settings_clear();
void settings_state();

/* voice.cpp: the Phone page: voice calls, the answering machine and
   its messages */
HWND voice_page_new(HWND parent);
void voice_page_layout(HWND page, int w, int h);
void voice_state();
void voice_event(const dmc_event *ev);
/* the answering machine's settings to the session */
void voice_configure();

/* audio.cpp: the sound card (waveOut and waveIn: Windows 95 has them),
   16-bit mono at 8 kHz */
/* the speaker's next samples: how many of n there were (the rest
   silence); the microphone's */
typedef int (*AudioPull)(short *pcm, int n);
typedef void (*AudioPush)(const short *pcm, int n);
/* sound from pull and to push (either NULL for none): false if there
   is no sound card */
bool audio_start(AudioPull pull, AudioPush push);
void audio_stop();

/* util */
/* UTF-8 to the ANSI code page (Windows 9x has few W functions) */
void to_ansi(const char *utf8, char *out, int n);
/* a code point to UTF-8: how many bytes */
int utf8_encode(unsigned long c, char *out);
HWND make_child(const char *cls, const char *text, DWORD style, HWND parent,
		int id, DWORD ex = 0);

#endif
