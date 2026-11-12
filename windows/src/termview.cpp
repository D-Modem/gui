/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * termview.cpp - a terminal control: a screen of libdmodemctl's dmc_vt,
 *                drawn with GDI, its keys handed to whoever owns it.
 *                The Remote page's terminal; each control has a screen
 *                of its own, so that there can be more than one.
 *
 * The text is drawn with ExtTextOutW, which Windows 95 has.  Keys are a
 * VT100's; the right button pastes; Backspace is DEL unless the owner
 * says otherwise.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"

struct TermView {
	HWND w;
	dmc_vt *vt;
	int cw, ch;             /* a cell's size */
	int scroll;             /* 0 the screen, -n back in history */
	bool focused;
	char backspace;
	TermTypedFn fn;
	void *ctx;
};

static const COLORREF BG = RGB(0x0a, 0x0f, 0x1c), FG = RGB(0xe8, 0xf0, 0xff);

static TermView *of(HWND w)
{
	return (TermView *)GetWindowLongPtr(w, GWLP_USERDATA);
}

static COLORREF colour(int c, COLORREF def)
{
	if (c < 0)
		return def;
	unsigned long v = dmc_vt_palette(c);
	return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

static void update_scrollbar(TermView *t)
{
	SCROLLINFO si;
	memset(&si, 0, sizeof(si));
	si.cbSize = sizeof(si);
	si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
	si.nMin = 0;
	si.nMax = dmc_vt_history(t->vt) + dmc_vt_rows(t->vt) - 1;
	si.nPage = dmc_vt_rows(t->vt);
	si.nPos = dmc_vt_history(t->vt) + t->scroll;
	SetScrollInfo(t->w, SB_VERT, &si, TRUE);
}

static void place_caret(TermView *t)
{
	if (!t->focused)
		return;
	if (t->scroll == 0 && dmc_vt_cursor_visible(t->vt)) {
		SetCaretPos(dmc_vt_cursor_col(t->vt) * t->cw,
			    dmc_vt_cursor_row(t->vt) * t->ch + t->ch - 2);
		ShowCaret(t->w);
	} else {
		HideCaret(t->w);
	}
}

static void paint(TermView *t, HDC dc, RECT *area)
{
	RECT r;
	GetClientRect(t->w, &r);
	HDC mem = CreateCompatibleDC(dc);
	HBITMAP bmp = CreateCompatibleBitmap(dc, r.right, r.bottom);
	HGDIOBJ oldb = SelectObject(mem, bmp);
	HGDIOBJ oldf = SelectObject(mem, app.mono_font);
	HBRUSH bg = CreateSolidBrush(BG);
	FillRect(mem, &r, bg);
	DeleteObject(bg);
	dmc_vt *vt = t->vt;
	int rows = dmc_vt_rows(vt), cols = dmc_vt_cols(vt);
	int cw = t->cw, ch = t->ch;
	WCHAR text[512];
	INT dx[512];
	for (int y = 0; y < rows; y++) {
		int row = y + t->scroll;
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
				dx[n] = cw;
				n++;
			}
			int f = fg >= 0 && fg < 8 && (at & DMC_BOLD) ? fg + 8 : fg;
			COLORREF fc = colour(f, FG), bc = colour(bgc, BG);
			if (at & DMC_REVERSE) {
				COLORREF s = fc;
				fc = bc;
				bc = s;
			}
			SetTextColor(mem, fc);
			SetBkColor(mem, bc);
			RECT cell = { x * cw, y * ch, (x + n) * cw, (y + 1) * ch };
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

static void measure(TermView *t)
{
	HDC dc = GetDC(t->w);
	HGDIOBJ old = SelectObject(dc, app.mono_font);
	TEXTMETRIC tm;
	GetTextMetrics(dc, &tm);
	t->cw = tm.tmAveCharWidth > 0 ? tm.tmAveCharWidth : 8;
	t->ch = tm.tmHeight > 0 ? tm.tmHeight : 16;
	SelectObject(dc, old);
	ReleaseDC(t->w, dc);
}

static void send_text(TermView *t, const char *s, int n)
{
	if (t->fn && n > 0)
		t->fn(s, n, t->ctx);
}

/* a character the keyboard gave, in the ANSI code page, as UTF-8 */
static void send_char(TermView *t, unsigned char c)
{
	if (c == 0x08)
		c = (unsigned char)t->backspace;
	if (c < 0x80) {
		char b = (char)c;
		send_text(t, &b, 1);
		return;
	}
	WCHAR w;
	char mb = (char)c;
	if (MultiByteToWideChar(CP_ACP, 0, &mb, 1, &w, 1) != 1)
		return;
	char o[4];
	send_text(t, o, utf8_encode(w, o));
}

static void paste(TermView *t)
{
	if (!OpenClipboard(t->w))
		return;
	HANDLE h = GetClipboardData(CF_TEXT);
	if (h) {
		const char *s = (const char *)GlobalLock(h);
		if (s) {
			for (const char *p = s; *p; p++) {
				if (*p == '\n' && p > s && p[-1] == '\r')
					continue;
				send_char(t, *p == '\n' ? '\r' : (unsigned char)*p);
			}
			GlobalUnlock(h);
		}
	}
	CloseClipboard();
}

static void to_bottom(TermView *t)
{
	if (t->scroll) {
		t->scroll = 0;
		update_scrollbar(t);
		InvalidateRect(t->w, NULL, FALSE);
	}
}

static void send_key(TermView *t, int key)
{
	char b[16];
	int n = dmc_vt_key(t->vt, key, b, sizeof(b));
	to_bottom(t);
	send_text(t, b, n);
}

static LRESULT CALLBACK view_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
	TermView *t = of(w);
	if (!t)
		return DefWindowProc(w, m, wp, lp);
	switch (m) {
	case WM_GETDLGCODE:
		return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTCHARS |
			DLGC_WANTTAB;
	case WM_SETFOCUS:
		t->focused = true;
		CreateCaret(w, NULL, t->cw, 2);
		place_caret(t);
		return 0;
	case WM_KILLFOCUS:
		t->focused = false;
		DestroyCaret();
		return 0;
	case WM_LBUTTONDOWN:
		SetFocus(w);
		return 0;
	case WM_RBUTTONUP:
		paste(t);
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(w, &ps);
		paint(t, dc, &ps.rcPaint);
		EndPaint(w, &ps);
		return 0;
	}
	case WM_SIZE: {
		int cols = LOWORD(lp) / t->cw, rows = HIWORD(lp) / t->ch;
		if (cols < 20)
			cols = 20;
		if (rows < 5)
			rows = 5;
		if (cols > 200)
			cols = 200;
		if (rows > 80)
			rows = 80;
		dmc_vt_resize(t->vt, cols, rows);
		update_scrollbar(t);
		InvalidateRect(w, NULL, FALSE);
		return 0;
	}
	case WM_VSCROLL: {
		int hist = dmc_vt_history(t->vt), pos = hist + t->scroll;
		switch (LOWORD(wp)) {
		case SB_LINEUP: pos--; break;
		case SB_LINEDOWN: pos++; break;
		case SB_PAGEUP: pos -= dmc_vt_rows(t->vt); break;
		case SB_PAGEDOWN: pos += dmc_vt_rows(t->vt); break;
		case SB_THUMBTRACK:
		case SB_THUMBPOSITION: pos = HIWORD(wp); break;
		case SB_TOP: pos = 0; break;
		case SB_BOTTOM: pos = hist; break;
		}
		if (pos < 0)
			pos = 0;
		if (pos > hist)
			pos = hist;
		t->scroll = pos - hist;
		update_scrollbar(t);
		place_caret(t);
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
				paste(t);
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
		send_key(t, key);
		return 0;
	}
	case WM_SYSKEYDOWN:
		if (wp == VK_F10) {
			send_key(t, DMC_KEY_F10);
			return 0;
		}
		break;
	case WM_SYSCHAR:
		/* Alt and a letter: ESC and it */
		if (wp >= 0x20 && wp < 0x7f) {
			char b[2] = { 0x1b, (char)wp };
			to_bottom(t);
			send_text(t, b, 2);
			return 0;
		}
		break;
	case WM_CHAR:
		if (wp == '\t' && GetKeyState(VK_SHIFT) < 0)
			return 0;
		to_bottom(t);
		send_char(t, (unsigned char)wp);
		return 0;
	case WM_NCDESTROY:
		SetWindowLongPtr(w, GWLP_USERDATA, 0);
		dmc_vt_free(t->vt);
		free(t);
		return 0;
	}
	return DefWindowProc(w, m, wp, lp);
}

HWND termview_new(HWND parent, int id, TermTypedFn fn, void *ctx)
{
	static bool registered;
	if (!registered) {
		WNDCLASS wc;
		memset(&wc, 0, sizeof(wc));
		wc.style = CS_HREDRAW | CS_VREDRAW;
		wc.lpfnWndProc = view_proc;
		wc.hInstance = app.inst;
		wc.hCursor = LoadCursor(NULL, IDC_IBEAM);
		wc.lpszClassName = "DModemTermView";
		RegisterClass(&wc);
		registered = true;
	}
	TermView *t = (TermView *)calloc(1, sizeof(TermView));
	t->vt = dmc_vt_new(80, 24, 2000);
	t->cw = 8;
	t->ch = 16;
	t->backspace = 0x7f;
	t->fn = fn;
	t->ctx = ctx;
	HWND w = make_child("DModemTermView", "", WS_TABSTOP | WS_VSCROLL,
			    parent, id, WS_EX_CLIENTEDGE);
	t->w = w;
	SetWindowLongPtr(w, GWLP_USERDATA, (LONG_PTR)t);
	measure(t);
	return w;
}

void termview_feed(HWND w, const char *data, int n)
{
	TermView *t = of(w);
	if (!t || n <= 0)
		return;
	int before = dmc_vt_history(t->vt);
	dmc_vt_feed(t->vt, data, n);
	if (t->scroll)
		t->scroll -= dmc_vt_history(t->vt) - before;
	char r[64];
	int k;
	while ((k = dmc_vt_reply(t->vt, r, sizeof(r))) > 0)
		send_text(t, r, k);
	update_scrollbar(t);
	place_caret(t);
	InvalidateRect(w, NULL, FALSE);
}

void termview_reset(HWND w)
{
	TermView *t = of(w);
	if (!t)
		return;
	dmc_vt_reset(t->vt);
	t->scroll = 0;
	update_scrollbar(t);
	InvalidateRect(w, NULL, FALSE);
}

void termview_size(HWND w, int *cols, int *rows)
{
	TermView *t = of(w);
	*cols = t ? dmc_vt_cols(t->vt) : 80;
	*rows = t ? dmc_vt_rows(t->vt) : 24;
}

void termview_set_backspace(HWND w, char b)
{
	TermView *t = of(w);
	if (t)
		t->backspace = b;
}
