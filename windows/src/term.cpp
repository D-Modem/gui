/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026, D-Modem
 *
 * term.cpp - the Terminal page: the modem's screen (libdmodemctl's
 *            dmc_vt, drawn with GDI), with buttons for its full-screen
 *            and command-line setup
 *
 * The text is drawn with ExtTextOutW, one of the few wide functions
 * Windows 95 has, so the configurator's box drawing shows on every
 * version.  Keys go to the modem as a VT100's; the right button pastes.
 */
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "resource.h"


static HWND page, term;
static dmc_vt *vt;
static int cw = 8, ch = 16;             /* a cell's size */
static int scroll;                      /* 0 the screen, -n back in history */
static bool focused;

/* the brand's background and text (D-Modem/brand: docs/palette.md) */
static const COLORREF BG = RGB(0x0a, 0x0f, 0x1c), FG = RGB(0xe8, 0xf0, 0xff);

static COLORREF colour(int c, COLORREF def)
{
	if (c < 0)
		return def;
	unsigned long v = dmc_vt_palette(c);
	return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

static void update_scrollbar()
{
	SCROLLINFO si;
	memset(&si, 0, sizeof(si));
	si.cbSize = sizeof(si);
	si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
	si.nMin = 0;
	si.nMax = dmc_vt_history(vt) + dmc_vt_rows(vt) - 1;
	si.nPage = dmc_vt_rows(vt);
	si.nPos = dmc_vt_history(vt) + scroll;
	SetScrollInfo(term, SB_VERT, &si, TRUE);
}

static void place_caret()
{
	if (!focused)
		return;
	if (scroll == 0 && dmc_vt_cursor_visible(vt)) {
		SetCaretPos(dmc_vt_cursor_col(vt) * cw,
			    dmc_vt_cursor_row(vt) * ch + ch - 2);
		ShowCaret(term);
	} else {
		HideCaret(term);
	}
}

static void paint(HDC dc, RECT *area)
{
	RECT r;
	GetClientRect(term, &r);
	HDC mem = CreateCompatibleDC(dc);
	HBITMAP bmp = CreateCompatibleBitmap(dc, r.right, r.bottom);
	HGDIOBJ oldb = SelectObject(mem, bmp);
	HGDIOBJ oldf = SelectObject(mem, app.mono_font);
	HBRUSH bg = CreateSolidBrush(BG);
	FillRect(mem, &r, bg);
	DeleteObject(bg);
	int rows = dmc_vt_rows(vt), cols = dmc_vt_cols(vt);
	WCHAR text[512];
	for (int y = 0; y < rows; y++) {
		int row = y + scroll;
		int x = 0;
		while (x < cols) {
			int fg = dmc_vt_fg(vt, row, x), bgc = dmc_vt_bg(vt, row, x);
			int at = dmc_vt_attr(vt, row, x);
			int n = 0;
			while (x + n < cols && n < 511 &&
			       dmc_vt_fg(vt, row, x + n) == fg &&
			       dmc_vt_bg(vt, row, x + n) == bgc &&
			       dmc_vt_attr(vt, row, x + n) == at) {
				unsigned long c = dmc_vt_char(vt, row, x + n);
				text[n] = c < 0x10000 ? (WCHAR)c : (WCHAR)'?';
				n++;
			}
			/* bold as the bright colours, as a PC's text mode */
			int f = fg >= 0 && fg < 8 && (at & DMC_BOLD) ? fg + 8 : fg;
			COLORREF fc = colour(f, FG), bc = colour(bgc, BG);
			if (at & DMC_REVERSE) {
				COLORREF t = fc;
				fc = bc;
				bc = t;
			}
			SetTextColor(mem, fc);
			SetBkColor(mem, bc);
			RECT cell = { x * cw, y * ch, (x + n) * cw, (y + 1) * ch };
			/* each character in its cell, whatever the font */
			INT dx[512];
			for (int i = 0; i < n; i++)
				dx[i] = cw;
			ExtTextOutW(mem, x * cw, y * ch, ETO_OPAQUE | ETO_CLIPPED,
				    &cell, text, n, dx);
			if (at & DMC_UNDERLINE) {
				RECT u = { x * cw, (y + 1) * ch - 1, (x + n) * cw,
					   (y + 1) * ch };
				HBRUSH b = CreateSolidBrush(fc);
				FillRect(mem, &u, b);
				DeleteObject(b);
			}
			x += n;
		}
	}
	BitBlt(dc, area->left, area->top, area->right - area->left,
	       area->bottom - area->top, mem, area->left, area->top, SRCCOPY);
	SelectObject(mem, oldf);
	SelectObject(mem, oldb);
	DeleteObject(bmp);
	DeleteDC(mem);
}

static void measure()
{
	HDC dc = GetDC(term);
	HGDIOBJ old = SelectObject(dc, app.mono_font);
	TEXTMETRIC tm;
	GetTextMetrics(dc, &tm);
	cw = tm.tmAveCharWidth > 0 ? tm.tmAveCharWidth : 8;
	ch = tm.tmHeight > 0 ? tm.tmHeight : 16;
	SelectObject(dc, old);
	ReleaseDC(term, dc);
}

static void send_text(const char *s, int n)
{
	link_type(s, n);
}

/* a character the keyboard gave, in the ANSI code page, as UTF-8 */
static void send_char(unsigned char c)
{
	if (c < 0x80) {
		char b = (char)c;
		send_text(&b, 1);
		return;
	}
	WCHAR w;
	char mb = (char)c;
	if (MultiByteToWideChar(CP_ACP, 0, &mb, 1, &w, 1) != 1)
		return;
	char o[4];
	send_text(o, utf8_encode(w, o));
}

static void paste()
{
	if (!OpenClipboard(term))
		return;
	HANDLE h = GetClipboardData(CF_TEXT);
	if (h) {
		const char *t = (const char *)GlobalLock(h);
		if (t) {
			for (const char *p = t; *p; p++) {
				/* a CR LF as the one CR a terminal sends */
				if (*p == '\n' && p > t && p[-1] == '\r')
					continue;
				send_char(*p == '\n' ? '\r' : (unsigned char)*p);
			}
			GlobalUnlock(h);
		}
	}
	CloseClipboard();
}

static void to_bottom()
{
	if (scroll) {
		scroll = 0;
		update_scrollbar();
		InvalidateRect(term, NULL, FALSE);
	}
}

static LRESULT CALLBACK term_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
	switch (m) {
	case WM_GETDLGCODE:
		return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTCHARS |
			DLGC_WANTTAB;
	case WM_SETFOCUS:
		focused = true;
		CreateCaret(w, NULL, cw, 2);
		place_caret();
		return 0;
	case WM_KILLFOCUS:
		focused = false;
		DestroyCaret();
		return 0;
	case WM_LBUTTONDOWN:
		SetFocus(w);
		return 0;
	case WM_RBUTTONUP:
		paste();
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(w, &ps);
		paint(dc, &ps.rcPaint);
		EndPaint(w, &ps);
		return 0;
	}
	case WM_SIZE: {
		int cols = LOWORD(lp) / cw, rows = HIWORD(lp) / ch;
		if (cols < 20)
			cols = 20;
		if (rows < 5)
			rows = 5;
		if (cols > 160)
			cols = 160;
		if (rows > 66)
			rows = 66;
		dmc_vt_resize(vt, cols, rows);
		update_scrollbar();
		InvalidateRect(w, NULL, FALSE);
		return 0;
	}
	case WM_VSCROLL: {
		int hist = dmc_vt_history(vt), pos = hist + scroll;
		switch (LOWORD(wp)) {
		case SB_LINEUP: pos--; break;
		case SB_LINEDOWN: pos++; break;
		case SB_PAGEUP: pos -= dmc_vt_rows(vt); break;
		case SB_PAGEDOWN: pos += dmc_vt_rows(vt); break;
		case SB_THUMBTRACK:
		case SB_THUMBPOSITION: pos = HIWORD(wp); break;
		case SB_TOP: pos = 0; break;
		case SB_BOTTOM: pos = hist; break;
		}
		if (pos < 0)
			pos = 0;
		if (pos > hist)
			pos = hist;
		scroll = pos - hist;
		update_scrollbar();
		place_caret();
		InvalidateRect(w, NULL, FALSE);
		return 0;
	}
	case WM_KEYDOWN: {
		int key = 0;
		bool shift = GetKeyState(VK_SHIFT) < 0;
		switch (wp) {
		case VK_UP: key = DMC_KEY_UP; break;
		case VK_DOWN: key = DMC_KEY_DOWN; break;
		case VK_LEFT: key = DMC_KEY_LEFT; break;
		case VK_RIGHT: key = DMC_KEY_RIGHT; break;
		case VK_HOME: key = DMC_KEY_HOME; break;
		case VK_END: key = DMC_KEY_END; break;
		case VK_PRIOR:
			if (shift) {
				SendMessage(w, WM_VSCROLL, SB_PAGEUP, 0);
				return 0;
			}
			key = DMC_KEY_PGUP;
			break;
		case VK_NEXT:
			if (shift) {
				SendMessage(w, WM_VSCROLL, SB_PAGEDOWN, 0);
				return 0;
			}
			key = DMC_KEY_PGDN;
			break;
		case VK_INSERT:
			if (shift) {
				paste();
				return 0;
			}
			key = DMC_KEY_INSERT;
			break;
		case VK_DELETE: key = DMC_KEY_DELETE; break;
		case VK_TAB:
			if (shift)
				key = DMC_KEY_BACKTAB;
			break;
		default:
			if (wp >= VK_F1 && wp <= VK_F10)
				key = DMC_KEY_F1 + (int)(wp - VK_F1);
			break;
		}
		if (!key)
			break;
		char b[16];
		int n = dmc_vt_key(vt, key, b, sizeof(b));
		to_bottom();
		send_text(b, n);
		return 0;
	}
	case WM_SYSKEYDOWN:
		/* F10 is the configurator's menu, not Windows' */
		if (wp == VK_F10) {
			char b[16];
			int n = dmc_vt_key(vt, DMC_KEY_F10, b, sizeof(b));
			send_text(b, n);
			return 0;
		}
		break;
	case WM_SYSCHAR:
		/* Alt and a letter: ESC and it, as a terminal sends them */
		if (wp >= 0x20 && wp < 0x7f) {
			char b[2] = { 0x1b, (char)wp };
			to_bottom();
			send_text(b, 2);
			return 0;
		}
		break;
	case WM_CHAR:
		if (wp == '\t' && GetKeyState(VK_SHIFT) < 0)
			return 0;       /* (Shift-Tab went as a key) */
		to_bottom();
		send_char((unsigned char)wp);
		return 0;
	}
	return DefWindowProc(w, m, wp, lp);
}

void term_feed(const char *data, int n)
{
	if (!vt)
		return;
	int before = dmc_vt_history(vt);
	dmc_vt_feed(vt, data, n);
	/* (looking back: the view stays where it was) */
	if (scroll)
		scroll -= dmc_vt_history(vt) - before;
	char r[64];
	int k;
	while ((k = dmc_vt_reply(vt, r, sizeof(r))) > 0)
		send_text(r, k);
	update_scrollbar();
	place_caret();
	InvalidateRect(term, NULL, FALSE);
}

void term_state()
{
	if (!page)
		return;
	bool idle = dmc_state(app.s) == DMC_IDLE;
	EnableWindow(GetDlgItem(page, IDC_SETUP_CF), idle);
	EnableWindow(GetDlgItem(page, IDC_SETUP_CT), idle);
}

static void typed_command(const char *cmd)
{
	if (dmc_state(app.s) != DMC_IDLE)
		return;
	link_type(cmd, (int)strlen(cmd));
	SetFocus(term);
}

static void term_command(HWND p, int id, int code)
{
	(void)p;
	(void)code;
	switch (id) {
	case IDC_SETUP_CF:
		typed_command("AT!CF\r");
		break;
	case IDC_SETUP_CT:
		typed_command("AT!CT\r");
		break;
	case IDC_CLEAR:
		dmc_vt_reset(vt);
		scroll = 0;
		update_scrollbar();
		InvalidateRect(term, NULL, FALSE);
		SetFocus(term);
		break;
	}
}

HWND term_page_new(HWND parent)
{
	WNDCLASS wc;
	memset(&wc, 0, sizeof(wc));
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = term_proc;
	wc.hInstance = app.inst;
	wc.hCursor = LoadCursor(NULL, IDC_IBEAM);
	wc.lpszClassName = "DModemTerminal";
	RegisterClass(&wc);
	vt = dmc_vt_new(80, 25, 2000);
	page = page_new(parent, term_command);
	make_child("BUTTON", "&Full-Screen Setup", WS_TABSTOP, page, IDC_SETUP_CF);
	make_child("BUTTON", "&Command-Line Setup", WS_TABSTOP, page,
		   IDC_SETUP_CT);
	make_child("BUTTON", "C&lear", WS_TABSTOP, page, IDC_CLEAR);
	term = make_child("DModemTerminal", "", WS_TABSTOP | WS_VSCROLL, page,
			  IDC_TERM, WS_EX_CLIENTEDGE);
	measure();
	return page;
}

void term_page_layout(HWND p, int w, int h)
{
	int m = 8, bh = 24;
	MoveWindow(GetDlgItem(p, IDC_SETUP_CF), m, m, 140, bh, TRUE);
	MoveWindow(GetDlgItem(p, IDC_SETUP_CT), m + 148, m, 150, bh, TRUE);
	MoveWindow(GetDlgItem(p, IDC_CLEAR), w - m - 80, m, 80, bh, TRUE);
	MoveWindow(term, m, 2 * m + bh, w - 2 * m, h - 3 * m - bh, TRUE);
}
