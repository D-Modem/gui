/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * main.cpp - the main window: a tab for each page (Modem, Phone,
 *            Terminal, Remote, Settings, Call Stats, Log, as the macOS
 *            and Linux apps
 *            have them) and a status bar
 */
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "resource.h"

App app;

static HWND status_bar;

enum { PAGE_MODEM, PAGE_PHONE, PAGE_TERM, PAGE_REMOTE, PAGE_SETTINGS,
       PAGE_STATS, PAGE_LOG, PAGES };
static const char *const page_names[PAGES] = {
	"Modem", "Phone", "Terminal", "Remote", "Settings", "Call Stats",
	"Log"
};

HWND make_child(const char *cls, const char *text, DWORD style, HWND parent,
		int id, DWORD ex)
{
	HWND w = CreateWindowEx(ex, cls, text, WS_CHILD | WS_VISIBLE | style,
				0, 0, 10, 10, parent, (HMENU)(INT_PTR)id,
				app.inst, NULL);
	SendMessage(w, WM_SETFONT, (WPARAM)app.font, TRUE);
	return w;
}

int utf8_encode(unsigned long c, char *o)
{
	if (c < 0x80) {
		o[0] = (char)c;
		return 1;
	}
	if (c < 0x800) {
		o[0] = (char)(0xC0 | (c >> 6));
		o[1] = (char)(0x80 | (c & 0x3F));
		return 2;
	}
	if (c < 0x10000) {
		o[0] = (char)(0xE0 | (c >> 12));
		o[1] = (char)(0x80 | ((c >> 6) & 0x3F));
		o[2] = (char)(0x80 | (c & 0x3F));
		return 3;
	}
	o[0] = (char)(0xF0 | (c >> 18));
	o[1] = (char)(0x80 | ((c >> 12) & 0x3F));
	o[2] = (char)(0x80 | ((c >> 6) & 0x3F));
	o[3] = (char)(0x80 | (c & 0x3F));
	return 4;
}

/* (by hand: Windows 95 has no CP_UTF8) */
void to_ansi(const char *s, char *out, int n)
{
	int o = 0;
	const unsigned char *p = (const unsigned char *)s;
	while (*p && o + 2 < n) {
		unsigned long c = *p++;
		int more = 0;
		if (c >= 0xF0) {
			c &= 0x07;
			more = 3;
		} else if (c >= 0xE0) {
			c &= 0x0F;
			more = 2;
		} else if (c >= 0xC0) {
			c &= 0x1F;
			more = 1;
		}
		while (more-- && (*p & 0xC0) == 0x80)
			c = (c << 6) | (*p++ & 0x3F);
		if (c < 0x80) {
			out[o++] = (char)c;
			continue;
		}
		WCHAR w = c < 0x10000 ? (WCHAR)c : (WCHAR)'?';
		char mb[8];
		BOOL lossy = FALSE;
		int k = WideCharToMultiByte(CP_ACP, 0, &w, 1, mb, sizeof(mb),
					    NULL, &lossy);
		if (k <= 0 || lossy) {
			out[o++] = '?';
			continue;
		}
		for (int i = 0; i < k && o + 1 < n; i++)
			out[o++] = mb[i];
	}
	out[o] = '\0';
}

void app_message(const char *text)
{
	char a[512];
	to_ansi(text, a, sizeof(a));
	SendMessage(status_bar, SB_SETTEXT, 0, (LPARAM)a);
}

void app_log(const char *line)
{
	log_append(line);
}

/* when the call was connected (GetTickCount), 0 when not online; the
   seconds the status bar shows */
static DWORD online_at;
static long online_shown = -1;

/* the state in the status bar, with the connect timer when online, as
   HyperTerminal has it: "Online 0:01:05" */
static void show_state()
{
	char t[160];
	int st = dmc_state(app.s);
	if (link_running() && st == DMC_CLOSED) {
		strcpy(t, "Starting");
	} else if (online_at) {
		long s = (long)((GetTickCount() - online_at) / 1000);
		online_shown = s;
		_snprintf(t, sizeof(t), "%s %ld:%02ld:%02ld", dmc_state_name(st),
			  s / 3600, s / 60 % 60, s % 60);
	} else {
		_snprintf(t, sizeof(t), "%s", dmc_state_name(st));
	}
	t[sizeof(t) - 1] = '\0';
	SendMessage(status_bar, SB_SETTEXT, 1, (LPARAM)t);
}

/* the connect timer moved on a second */
static void state_tick()
{
	if (online_at && (long)((GetTickCount() - online_at) / 1000) != online_shown)
		show_state();
}

void app_state()
{
	int st = dmc_state(app.s);
	bool on = st == DMC_ONLINE || st == DMC_ONLINE_COMMAND;
	bool was_online = online_at != 0;
	if (on && !online_at)
		online_at = GetTickCount() ? GetTickCount() : 1;
	else if (!on)
		online_at = 0;
	show_state();
	/* Terminal Settings: Exit program upon disconnecting */
	if (was_online && !online_at && termset_exit_on_disconnect())
		PostMessage(app.main, WM_CLOSE, 0, 0);
	modem_page_state();
	voice_state();
	term_state();
	settings_state();
}

void app_opened()
{
	app_state();
	settings_load();
	stats_refresh();
}

void app_closed()
{
	settings_clear();
	app_state();
}

void app_event(const dmc_event *ev)
{
	switch (ev->type) {
	case DMC_EV_STATE: {
		int was = app.last_state;
		app.last_state = ev->state;
		app_state();
		if (ev->state == DMC_IDLE &&
		    (was == DMC_ONLINE || was == DMC_ONLINE_COMMAND ||
		     was == DMC_HANGING_UP || was == DMC_DIALING ||
		     was == DMC_ANSWERING))
			stats_refresh();
		if (ev->state == DMC_IDLE && was == DMC_CONFIGURATOR)
			settings_load();
		break;
	}
	case DMC_EV_RESULT:
		if (!strcmp(ev->result, "RING")) {
			app_message("Ringing");
			if (GetForegroundWindow() != app.main)
				FlashWindow(app.main, TRUE);
		} else if (ev->result[0]) {
			app_message(ev->result);
		}
		break;
	case DMC_EV_CALLER:
		app_state();
		break;
	case DMC_EV_VOICE:
	case DMC_EV_MESSAGE:
		voice_event(ev);
		break;
	}
}

#ifdef DMODEM_WINPORT
/* (Linux and macOS, winport/: the layer's hit test stops at a group box,
   which answers HTTRANSPARENT, and does not try the controls under it.
   Windows tries them. Until the layer does too, put the group boxes
   under the other controls.) */
static BOOL CALLBACK lower_group_box(HWND c, LPARAM lp)
{
	char cls[16];
	(void)lp;
	if (GetClassName(c, cls, sizeof(cls)) && !lstrcmpi(cls, "BUTTON") &&
	    (GetWindowLong(c, GWL_STYLE) & BS_TYPEMASK) == BS_GROUPBOX)
		SetWindowPos(c, HWND_BOTTOM, 0, 0, 0, 0,
			     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	return TRUE;
}
#endif

static int page_now;

/* the line of the page shown, for the Transfer menu: the Remote page's
   login, else the modem */
static XferLine *line_now()
{
	return page_now == PAGE_REMOTE ? &remote_line : &term_line;
}

/* the terminal of the page shown, for the Edit menu: NULL on a page
   with none */
static HWND view_now()
{
	if (page_now != PAGE_TERM && page_now != PAGE_REMOTE)
		return NULL;
	XferLine *l = line_now();
	return l->view ? *l->view : NULL;
}

static void show_page(int i)
{
	page_now = i;
	for (int p = 0; p < PAGES; p++)
		ShowWindow(app.pages[p], p == i ? SW_SHOW : SW_HIDE);
	if (i == PAGE_TERM)
		SetFocus(GetWindow(app.pages[i], GW_CHILD));
	else if (i == PAGE_REMOTE)
		remote_focus();
}

static void layout()
{
	RECT r;
	GetClientRect(app.main, &r);
	SendMessage(status_bar, WM_SIZE, 0, 0);
	RECT sb;
	GetWindowRect(status_bar, &sb);
	int sbh = sb.bottom - sb.top;
	int parts[2] = { r.right - 200, -1 };
	SendMessage(status_bar, SB_SETPARTS, 2, (LPARAM)parts);
	MoveWindow(app.tabs, 4, 4, r.right - 8, r.bottom - sbh - 8, TRUE);
	RECT in = { 0, 0, r.right - 8, r.bottom - sbh - 8 };
	TabCtrl_AdjustRect(app.tabs, FALSE, &in);
	int x = in.left + 4, y = in.top + 4;
	int w = in.right - in.left, h = in.bottom - in.top;
	for (int p = 0; p < PAGES; p++)
		MoveWindow(app.pages[p], x, y, w, h, TRUE);
	modem_page_layout(app.pages[PAGE_MODEM], w, h);
	voice_page_layout(app.pages[PAGE_PHONE], w, h);
	term_page_layout(app.pages[PAGE_TERM], w, h);
	remote_page_layout(app.pages[PAGE_REMOTE], w, h);
	settings_page_layout(app.pages[PAGE_SETTINGS], w, h);
	stats_page_layout(app.pages[PAGE_STATS], w, h);
	log_page_layout(app.pages[PAGE_LOG], w, h);
}

static void about()
{
	MessageBox(app.main,
		   "D-Modem 1.0.0\r\n\r\n"
		   "The front panel of D-Modem, the open V.92 softmodem.\r\n"
		   "https://github.com/D-Modem/gui\r\n\r\n"
		   "Copyright (C) 2026 D-Modem Authors. AGPL-3.0-only.",
		   "About D-Modem", MB_OK | MB_ICONINFORMATION);
}

static LRESULT CALLBACK main_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
	switch (m) {
	case WM_CREATE: {
		app.main = w;
		status_bar = CreateStatusWindow(WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
						"", w, IDC_STATUS);
		app.tabs = make_child(WC_TABCONTROL, "", WS_CLIPSIBLINGS, w,
				      IDC_TABS);
		for (int p = 0; p < PAGES; p++) {
			TCITEM it;
			memset(&it, 0, sizeof(it));
			it.mask = TCIF_TEXT;
			it.pszText = (char *)page_names[p];
			TabCtrl_InsertItem(app.tabs, p, &it);
		}
		app.pages[PAGE_MODEM] = modem_page_new(w);
		app.pages[PAGE_PHONE] = voice_page_new(w);
		app.pages[PAGE_TERM] = term_page_new(w);
		app.pages[PAGE_REMOTE] = remote_page_new(w);
		app.pages[PAGE_SETTINGS] = settings_page_new(w);
		app.pages[PAGE_STATS] = stats_page_new(w);
		app.pages[PAGE_LOG] = log_page_new(w);
		for (int p = 0; p < PAGES; p++)
			SetWindowPos(app.pages[p], HWND_TOP, 0, 0, 0, 0,
				     SWP_NOMOVE | SWP_NOSIZE);
#ifdef DMODEM_WINPORT
		for (int p = 0; p < PAGES; p++)
			EnumChildWindows(app.pages[p], lower_group_box, 0);
#endif
		show_page(PAGE_MODEM);
		SetTimer(w, TIMER_PUMP, 50, NULL);
		app_state();
		return 0;
	}
	case WM_SIZE:
		layout();
		return 0;
	case WM_GETMINMAXINFO: {
		MINMAXINFO *mm = (MINMAXINFO *)lp;
		mm->ptMinTrackSize.x = 560;
		mm->ptMinTrackSize.y = 420;
		return 0;
	}
	case WM_NOTIFY: {
		NMHDR *n = (NMHDR *)lp;
		if (n->hwndFrom == app.tabs && n->code == TCN_SELCHANGE)
			show_page(TabCtrl_GetCurSel(app.tabs));
		break;
	}
	case WM_COMMAND:
		switch (LOWORD(wp)) {
		case IDM_LINE:
			if (line_dialog(w, &app.cfg)) {
				config_save(&app.cfg);
				app_state();
			}
			return 0;
		case IDM_ASCII:
			ascii_dialog(w);
			return 0;
		case IDM_TERMSET:
			termset_dialog(w, line_now() == &remote_line);
			return 0;
		case IDM_BREAK:
			if (line_now() == &remote_line ? remote_break() : link_break())
				app_message("Break sent");
			else
				app_message(line_now() == &remote_line ?
					    "This connection cannot send a break (Telnet only)" :
					    "This line cannot send a break (a COM port only)");
			return 0;
		case IDM_START:
			if (link_running())
				link_stop();
			else
				link_start();
			return 0;
		case IDM_EXIT:
			DestroyWindow(w);
			return 0;
		case IDM_ABOUT:
			about();
			return 0;
		case IDM_SEND:
			xfer_send_dialog(line_now(), -1);
			return 0;
		case IDM_RECEIVE:
			xfer_receive_dialog(line_now());
			return 0;
		case IDM_AUTORECV:
			xfer_set_auto(!xfer_auto());
			return 0;
		case IDM_XCANCEL:
			xfer_cancel();
			return 0;
		case IDM_EDCOPY:
			if (view_now())
				termview_copy(view_now());
			return 0;
		case IDM_EDPASTE:
			if (view_now())
				termview_paste(view_now());
			return 0;
		case IDM_SELALL:
			if (view_now())
				termview_select_all(view_now());
			return 0;
		case IDM_FPRINT:
			capture_print(line_now());
			return 0;
		case IDM_CAPTURE:
			capture_text_start(line_now());
			return 0;
		case IDM_CAPPAUSE:
			capture_text_pause(line_now(),
					   capture_text_state(line_now()) == CAPTURE_ON);
			return 0;
		case IDM_CAPSTOP:
			capture_text_stop(line_now());
			return 0;
		case IDM_CAPPRINT:
			capture_printer(line_now(),
					capture_printer_state(line_now()) == CAPTURE_OFF);
			return 0;
		case IDM_PRNPAUSE:
			capture_printer_pause(line_now(),
					      capture_printer_state(line_now()) == CAPTURE_ON);
			return 0;
		}
		break;
	case WM_INITMENUPOPUP: {
		HMENU m = (HMENU)wp;
		bool busy = xfer_running();
		EnableMenuItem(m, IDM_SEND, MF_BYCOMMAND | (busy ? MF_GRAYED : MF_ENABLED));
		EnableMenuItem(m, IDM_RECEIVE, MF_BYCOMMAND | (busy ? MF_GRAYED : MF_ENABLED));
		EnableMenuItem(m, IDM_XCANCEL, MF_BYCOMMAND | (busy ? MF_ENABLED : MF_GRAYED));
		CheckMenuItem(m, IDM_AUTORECV, MF_BYCOMMAND |
			      (xfer_auto() ? MF_CHECKED : MF_UNCHECKED));
		HWND v = view_now();
		UINT has_view = v ? MF_ENABLED : MF_GRAYED;
		EnableMenuItem(m, IDM_EDCOPY, MF_BYCOMMAND |
			       (v && termview_has_selection(v) ? MF_ENABLED : MF_GRAYED));
		EnableMenuItem(m, IDM_EDPASTE, MF_BYCOMMAND | has_view);
		EnableMenuItem(m, IDM_SELALL, MF_BYCOMMAND | has_view);
		EnableMenuItem(m, IDM_FPRINT, MF_BYCOMMAND | has_view);
		XferLine *l = line_now();
		int ct = capture_text_state(l), cp = capture_printer_state(l);
		EnableMenuItem(m, IDM_CAPTURE, MF_BYCOMMAND |
			       (ct == CAPTURE_OFF ? MF_ENABLED : MF_GRAYED));
		EnableMenuItem(m, IDM_CAPPAUSE, MF_BYCOMMAND |
			       (ct != CAPTURE_OFF ? MF_ENABLED : MF_GRAYED));
		CheckMenuItem(m, IDM_CAPPAUSE, MF_BYCOMMAND |
			      (ct == CAPTURE_PAUSED ? MF_CHECKED : MF_UNCHECKED));
		EnableMenuItem(m, IDM_CAPSTOP, MF_BYCOMMAND |
			       (ct != CAPTURE_OFF ? MF_ENABLED : MF_GRAYED));
		CheckMenuItem(m, IDM_CAPPRINT, MF_BYCOMMAND |
			      (cp != CAPTURE_OFF ? MF_CHECKED : MF_UNCHECKED));
		EnableMenuItem(m, IDM_PRNPAUSE, MF_BYCOMMAND |
			       (cp != CAPTURE_OFF ? MF_ENABLED : MF_GRAYED));
		CheckMenuItem(m, IDM_PRNPAUSE, MF_BYCOMMAND |
			      (cp == CAPTURE_PAUSED ? MF_CHECKED : MF_UNCHECKED));
		break;
	}
	case WM_TIMER:
		if (wp == TIMER_PUMP || wp == TIMER_VOICE)
			link_pump();
		else if (wp == TIMER_REMOTE)
			remote_tick();
		xfer_pump();
		capture_tick();
		state_tick();
		return 0;
	case WM_APP_XFER:
		xfer_message(wp, lp);
		return 0;
	case WM_APP_SOCKET:
		link_socket(wp, lp);
		return 0;
	case WM_APP_REMOTE:
		remote_socket(wp, lp);
		return 0;
	case WM_APP_SSH:
		remote_ssh(lp);
		return 0;
	case WM_DESTROY:
		KillTimer(w, TIMER_PUMP);
		KillTimer(w, TIMER_VOICE);
		audio_stop();
		remote_stop();
		capture_close_all();
		if (link_running())
			link_stop();
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProc(w, m, wp, lp);
}

static HFONT font(const char *face, int points, int weight)
{
	HDC dc = GetDC(NULL);
	int h = -MulDiv(points, GetDeviceCaps(dc, LOGPIXELSY), 72);
	ReleaseDC(NULL, dc);
	return CreateFont(h, 0, 0, 0, weight, FALSE, FALSE, FALSE,
			  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
			  CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
			  FIXED_PITCH | FF_MODERN, face);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
	(void)prev;
	(void)cmd;
	app.inst = inst;
	InitCommonControls();
	WSADATA wsa;
	WSAStartup(MAKEWORD(1, 1), &wsa);

	/* the system's dialog font (MS Sans Serif on 9x, Tahoma or Segoe
	   UI later), bold and large versions of it, and a terminal font */
	NONCLIENTMETRICS ncm;
	memset(&ncm, 0, sizeof(ncm));
	ncm.cbSize = sizeof(ncm);
	if (SystemParametersInfo(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
		app.font = CreateFontIndirect(&ncm.lfMessageFont);
	else
		app.font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
	LOGFONT lf;
	GetObject(app.font, sizeof(lf), &lf);
	lf.lfWeight = FW_BOLD;
	app.bold_font = CreateFontIndirect(&lf);
	lf.lfHeight = lf.lfHeight * 2;
	app.big_font = CreateFontIndirect(&lf);
	app.mono_font = font("Lucida Console", 10, FW_NORMAL);
	if (!app.mono_font)
		app.mono_font = font("Courier New", 10, FW_NORMAL);

	config_load(&app.cfg);
	app.s = dmc_session_new();

	WNDCLASS wc;
	memset(&wc, 0, sizeof(wc));
	wc.lpfnWndProc = main_proc;
	wc.hInstance = inst;
	wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(IDI_APP));
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	wc.lpszMenuName = MAKEINTRESOURCE(IDR_MENU);
	wc.lpszClassName = "DModemMain";
	if (!RegisterClass(&wc))
		return 1;
	HWND w = CreateWindowEx(0, "DModemMain", "D-Modem",
				WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
				CW_USEDEFAULT, CW_USEDEFAULT, 720, 560, NULL,
				NULL, inst, NULL);
	if (!w)
		return 1;
	ShowWindow(w, show);
	UpdateWindow(w);
	/* a modem that is set up starts at once */
	if (app.cfg.line != LINE_DMODEMD || config_sip_ready(&app.cfg) ||
	    app.cfg.usb[0])
		link_start();

	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0) > 0) {
		if (xfer_dialog_key(&msg))
			continue;
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
	dmc_session_free(app.s);
	WSACleanup();
	return (int)msg.wParam;
}

#ifdef __PICOLIBC__
/* (picolibc's start-up code needs a main, also when the entry point is
   WinMainCRTStartup, which calls WinMain: see windows/CMakeLists.txt) */
int main(void)
{
	return WinMain(GetModuleHandleA(NULL), NULL, (LPSTR)"", SW_SHOWDEFAULT);
}
#endif
