/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026, D-Modem
 *
 * app.h - the Windows app: plain Win32 in C++98, ANSI, the common
 *         controls of Windows 95, so the one .exe runs on Windows 95,
 *         98 and ME and on NT 4 through Windows 11.  Built with Open
 *         Watcom (windows/build-watcom.sh), or MinGW or MSVC (CMake).
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
	int listen, t38, v152, v150;
	char country[64];
	char usb[64];                   /* --usb, "" for a SIP line */
	char dmodemd[MAX_PATH];         /* dmodemd.exe, "" next to the app */
	char tcp[256];                  /* host:port */
	char com[32];                   /* COM1 */
	char init[256];                 /* commands when the modem opens */
};

void config_load(Config *c);
void config_save(const Config *c);
/* "SIP: alice@sip.example.com" and the like, into out */
void config_summary(const Config *c, char *out, int n);
/* the Line Settings dialog: true if changed */
bool line_dialog(HWND owner, Config *c);

/* a command's response */
typedef void (*ResponseFn)(const dmc_event *ev, void *data);

/* the app's state */
struct App {
	HINSTANCE inst;
	HWND main, tabs;
	HWND pages[5];
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
/* the session's output written, input read, events handled */
void link_pump();
/* socket messages (WM_APP_SOCKET) */
void link_socket(WPARAM w, LPARAM l);
void link_type(const char *data, int n);
int link_command(const char *cmd, int flags, ResponseFn fn, void *data);

#define WM_APP_SOCKET (WM_APP + 1)
#define TIMER_PUMP 1

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

/* settings.cpp: the Settings page */
HWND settings_page_new(HWND parent);
void settings_page_layout(HWND page, int w, int h);
void settings_load();
void settings_clear();
void settings_state();

/* util */
/* UTF-8 to the ANSI code page (Windows 9x has few W functions) */
void to_ansi(const char *utf8, char *out, int n);
/* a code point to UTF-8: how many bytes */
int utf8_encode(unsigned long c, char *out);
HWND make_child(const char *cls, const char *text, DWORD style, HWND parent,
		int id, DWORD ex = 0);

#endif
