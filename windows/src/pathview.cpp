/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * pathview.cpp - the connection path at the top of the Settings page, as
 *                PCOMM's "Connection Overview" draws its link: each step's
 *                role above its icon, what it is below, one line joining
 *                them (libdmodemctl's dmc_path), in plain GDI for Windows
 *                95 and later.  Idle, how the line settings set it up;
 *                starting, calling and online, as it is now.
 */
#include <string.h>

#include "app.h"

static dmc_path *path;
static HWND view;
static HFONT glyph_font;

/* a state's colour: ready the highlight's, to set red, working and
   active green, off grey */
static COLORREF state_color(int st)
{
	switch (st) {
	case DMC_STEP_SETUP: return RGB(192, 0, 0);
	case DMC_STEP_WORKING:
	case DMC_STEP_ACTIVE: return RGB(0, 128, 0);
	case DMC_STEP_OFF: return GetSysColor(COLOR_GRAYTEXT);
	default: return GetSysColor(COLOR_HIGHLIGHT);
	}
}

/* a step's mark in its ring: a Wingdings picture (as Windows 95 has
   them), or the port's name */
static const char *mark(int kind, bool *wingdings)
{
	*wingdings = true;
	switch (kind) {
	case DMC_KIND_TERMINAL: return ":";      /* a computer */
	case DMC_KIND_SOFTMODEM:
	case DMC_KIND_MODEM: return ";";         /* a box */
	case DMC_KIND_SIP: return ")";           /* a telephone, ringed */
	case DMC_KIND_SIP_ADDRESS: return "*";   /* an envelope */
	case DMC_KIND_PHONE_NETWORK:
	case DMC_KIND_PHONE_LINE: return "(";    /* a telephone */
	}
	*wingdings = false;
	switch (kind) {
	case DMC_KIND_USB: return "USB";
	case DMC_KIND_CODEC: return "ALSA";
	case DMC_KIND_NETWORK: return "TCP";
	default: return "COM";
	}
}

static void line(HDC dc, int st, int x0, int x1, int y)
{
	bool dashed = st == DMC_STEP_SETUP || st == DMC_STEP_OFF;
	/* (Windows 9x dashes only one pixel wide pens) */
	HPEN pen = CreatePen(dashed ? PS_DASH : PS_SOLID, dashed ? 1 : 2,
			     state_color(st));
	HGDIOBJ old = SelectObject(dc, pen);
	MoveToEx(dc, x0, y, NULL);
	LineTo(dc, x1, y);
	SelectObject(dc, old);
	DeleteObject(pen);
}

static void text(HDC dc, HFONT f, COLORREF c, const char *utf8, RECT *r,
		 UINT how)
{
	char a[512];
	to_ansi(utf8, a, sizeof(a));
	SelectObject(dc, f);
	SetTextColor(dc, c);
	DrawText(dc, a, -1, r, how | DT_NOPREFIX);
}

static void paint(HWND w)
{
	PAINTSTRUCT ps;
	HDC dc = BeginPaint(w, &ps);
	RECT rc;
	GetClientRect(w, &rc);
	FillRect(dc, &rc, (HBRUSH)(COLOR_BTNFACE + 1));
	SetBkMode(dc, TRANSPARENT);
	HGDIOBJ oldf = SelectObject(dc, app.font);
	TEXTMETRIC tm;
	GetTextMetrics(dc, &tm);
	int th = tm.tmHeight;
	int n = dmc_path_steps(path), cw = (rc.right - 8) / n;
	int r = 18, top = 4, cy = top + th + 4 + r;
	COLORREF grey = GetSysColor(COLOR_GRAYTEXT),
		 ink = GetSysColor(COLOR_BTNTEXT);
	for (int i = 0; i < n; i++) {
		int x = 4 + cw * i, cx = x + cw / 2;
		int st = dmc_path_state(path, i);
		if (i > 0)
			line(dc, dmc_path_link(path, i - 1), x, cx - r, cy);
		if (i < n - 1)
			line(dc, dmc_path_link(path, i), cx + r, x + cw, cy);
		RECT t = { x, top, x + cw, top + th };
		text(dc, app.font, grey, dmc_path_role(path, i), &t,
		     DT_CENTER | DT_SINGLELINE);
		/* the ring, filled in a call */
		HPEN pen = CreatePen(PS_SOLID, 2, state_color(st));
		HBRUSH br = st == DMC_STEP_ACTIVE ?
			CreateSolidBrush(RGB(214, 238, 214)) :
			CreateSolidBrush(GetSysColor(COLOR_WINDOW));
		HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, br);
		Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);
		SelectObject(dc, op);
		SelectObject(dc, ob);
		DeleteObject(pen);
		DeleteObject(br);
		bool wd;
		const char *m = mark(dmc_path_kind(path, i), &wd);
		RECT g = { cx - r, cy - r, cx + r, cy + r };
		SelectObject(dc, wd ? glyph_font : app.font);
		SetTextColor(dc, state_color(st));
		DrawText(dc, m, -1, &g, DT_CENTER | DT_VCENTER | DT_SINGLELINE |
			 DT_NOPREFIX);
		RECT tt = { x, cy + r + 4, x + cw, cy + r + 4 + th };
		text(dc, app.bold_font, ink, dmc_path_title(path, i), &tt,
		     DT_CENTER | DT_SINGLELINE);
		RECT d = { x + 2, tt.bottom + 2, x + cw - 2, tt.bottom + 2 + 2 * th };
		text(dc, app.font, st == DMC_STEP_SETUP ? state_color(st) : grey,
		     dmc_path_detail(path, i), &d,
		     DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
	}
	RECT h = { 4, rc.bottom - th - 4, rc.right - 4, rc.bottom - 4 };
	text(dc, app.font, dmc_path_needs_setup(path) ?
	     state_color(DMC_STEP_SETUP) : grey, dmc_path_headline(path), &h,
	     DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
	SelectObject(dc, oldf);
	EndPaint(w, &ps);
}

static LRESULT CALLBACK path_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_PAINT:
		paint(w);
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_SIZE:
		InvalidateRect(w, NULL, FALSE);
		return 0;
	}
	return DefWindowProc(w, msg, wp, lp);
}

HWND pathview_new(HWND parent)
{
	WNDCLASS wc;
	memset(&wc, 0, sizeof(wc));
	wc.lpfnWndProc = path_proc;
	wc.hInstance = app.inst;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.lpszClassName = "DModemPath";
	RegisterClass(&wc);
	if (!glyph_font) {
		LOGFONT lf;
		memset(&lf, 0, sizeof(lf));
		lf.lfHeight = -20;
		lf.lfCharSet = SYMBOL_CHARSET;
		strcpy(lf.lfFaceName, "Wingdings");
		glyph_font = CreateFontIndirect(&lf);
	}
	path = dmc_path_new();
	view = CreateWindowEx(0, "DModemPath", "", WS_CHILD | WS_VISIBLE,
			      0, 0, 10, 10, parent, NULL, app.inst, NULL);
	pathview_update();
	return view;
}

int pathview_height()
{
	HDC dc = GetDC(view);
	HGDIOBJ old = SelectObject(dc, app.font);
	TEXTMETRIC tm;
	GetTextMetrics(dc, &tm);
	SelectObject(dc, old);
	ReleaseDC(view, dc);
	/* the role, the ring, the title, two lines of what it is, the
	   headline */
	return 4 + tm.tmHeight + 4 + 36 + 4 + tm.tmHeight + 2 +
		2 * tm.tmHeight + 6 + tm.tmHeight + 4;
}

void pathview_update()
{
	if (!path)
		return;
	const Config *c = &app.cfg;
	int kind = DMC_LINE_SIP;
	const char *where = "";
	if (c->line == LINE_TCP) {
		kind = DMC_LINE_TCP;
		where = c->tcp;
	} else if (c->line == LINE_COM) {
		kind = DMC_LINE_COM;
		where = c->com;
	} else if (c->usb[0]) {
		kind = DMC_LINE_USB;
		where = c->usb;
	}
	dmc_path_set_line(path, kind, where, c->sip_anonymous, c->sip_user,
			  c->sip_server, c->listen);
	int st = app.s ? dmc_state(app.s) : DMC_CLOSED;
	dmc_path_set_live(path, link_running(), st,
			  st == DMC_ONLINE || st == DMC_ONLINE_COMMAND ?
			  dmc_connect_text(app.s) : "");
	char a[512];
	to_ansi(dmc_path_headline(path), a, sizeof(a));
	SetWindowText(view, a);         /* (for a screen reader) */
	InvalidateRect(view, NULL, FALSE);
}
