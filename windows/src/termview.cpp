/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * termview.cpp - a terminal control: a screen of libdmodemctl's dmc_vt,
 *                drawn with GDI, its keys handed to whoever owns it.  The
 *                Terminal page's and the Remote page's; each control has
 *                a screen of its own.
 *
 * Every kind of terminal the engine is (the profiles: xterm, the VTs,
 * ANSI-BBS, the Commodores and Ataris, Viewdata, the Minitel, the
 * Tektronix) is drawn here.  A row is drawn into a 32-bit DIB section:
 * its backgrounds and text with ExtTextOutW (Windows 95 has it), then the
 * cells a machine's font draws (their bitmaps, pixel by pixel), the lines
 * under and over, the pictures (sixel, ReGIS, RIPscrip, the Tektronix's)
 * over it with their transparency, and the cursor; then copied to the
 * screen, stretched for DEC's double width and height lines.  A machine
 * with a screen of its own size (a C64's 40x25) has its cells stretched
 * to fill the control.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"

#ifndef WM_MOUSEWHEEL
#define WM_MOUSEWHEEL 0x020A
#endif
#ifndef GET_WHEEL_DELTA_WPARAM
#define GET_WHEEL_DELTA_WPARAM(w) ((short)HIWORD(w))
#endif

#define TIMER_BLINK 1
#define MAX_COLS 400

struct TermView {
	HWND w;
	dmc_vt *vt;
	int fcw, fch;           /* the font's cell */
	int cw, ch;             /* a cell as drawn */
	int ox, oy;             /* the screen's place in the control */
	int status_rows;        /* the status line's (0 or 1) */
	int scroll;             /* 0 the screen, -n back in history */
	bool focused, blink_off, has_blink;
	int buttons;            /* the mouse buttons down, for reports */
	TermTypedFn fn;
	void *ctx;
	/* the selection: from the anchor to the end, in rows as
	   dmc_vt_cell() has them (below 0 the scrollback) */
	bool sel, selecting;
	int ar, ac, er, ec;
	TermPrintedFn pfn;      /* what the remote prints goes to it */
	void *pctx;
	bool echo;              /* ASCII Setup: what is typed also shown */
	HFONT fonts[4];         /* regular, bold, italic, bold italic */
	/* a row being drawn */
	HDC row_dc;
	HBITMAP row_bmp;
	HGDIOBJ row_old;
	DWORD *row_px;
	int row_w, row_h;
};

static TermView *of(HWND w)
{
	return (TermView *)GetWindowLongPtr(w, GWLP_USERDATA);
}

static COLORREF cref(unsigned long v)
{
	return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

/* ------------------------------------------------------------------ */
/* ANSI music: the notes played one after the other on a thread of their
   own (Beep sounds them on NT; Windows 9x's only clicks) */

static CRITICAL_SECTION music_lock;
static bool music_init, music_playing;
static DWORD music_q[256][3];
static int music_head, music_n;

static DWORD WINAPI music_thread(LPVOID)
{
	for (;;) {
		DWORD n[3];
		EnterCriticalSection(&music_lock);
		if (!music_n) {
			music_playing = false;
			LeaveCriticalSection(&music_lock);
			return 0;
		}
		memcpy(n, music_q[music_head], sizeof(n));
		music_head = (music_head + 1) % 256;
		music_n--;
		LeaveCriticalSection(&music_lock);
		if (n[0] >= 37 && n[0] <= 32767)
			Beep(n[0], n[1]);
		else
			Sleep(n[1]);
		if (n[2])
			Sleep(n[2]);
	}
}

static void play_music(dmc_vt *vt)
{
	uint32_t hz100;
	int ms, rest;
	while (dmc_vt_music(vt, &hz100, &ms, &rest)) {
		if (!music_init) {
			InitializeCriticalSection(&music_lock);
			music_init = true;
		}
		EnterCriticalSection(&music_lock);
		if (music_n < 256) {
			DWORD *n = music_q[(music_head + music_n) % 256];
			n[0] = hz100 / 100;
			n[1] = (DWORD)ms;
			n[2] = (DWORD)rest;
			music_n++;
		}
		bool start = !music_playing;
		music_playing = true;
		LeaveCriticalSection(&music_lock);
		if (start) {
			DWORD id;
			HANDLE h = CreateThread(NULL, 0, music_thread, NULL, 0, &id);
			if (h)
				CloseHandle(h);
			else
				music_playing = false;
		}
	}
}

/* ------------------------------------------------------------------ */
/* the screen's size */

static void make_fonts(TermView *t)
{
	LOGFONT lf;
	GetObject(app.mono_font, sizeof(lf), &lf);
	t->fonts[0] = app.mono_font;
	lf.lfWeight = FW_BOLD;
	t->fonts[1] = CreateFontIndirect(&lf);
	lf.lfWeight = FW_NORMAL;
	lf.lfItalic = TRUE;
	t->fonts[2] = CreateFontIndirect(&lf);
	lf.lfWeight = FW_BOLD;
	t->fonts[3] = CreateFontIndirect(&lf);
	for (int i = 1; i < 4; i++)
		if (!t->fonts[i])
			t->fonts[i] = app.mono_font;
}

static void measure(TermView *t)
{
	HDC dc = GetDC(t->w);
	HGDIOBJ old = SelectObject(dc, app.mono_font);
	TEXTMETRIC tm;
	GetTextMetrics(dc, &tm);
	t->fcw = tm.tmAveCharWidth > 0 ? tm.tmAveCharWidth : 8;
	t->fch = tm.tmHeight > 0 ? tm.tmHeight : 16;
	SelectObject(dc, old);
	ReleaseDC(t->w, dc);
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

/* the columns and rows that fit, or the machine's own with its cells
   stretched to fill the control */
static void layout(TermView *t)
{
	RECT r;
	GetClientRect(t->w, &r);
	int W = r.right > 0 ? r.right : 1, H = r.bottom > 0 ? r.bottom : 1;
	int rc = dmc_vt_requested_cols(t->vt), rr = dmc_vt_requested_rows(t->vt);
	const char *st = dmc_vt_status_line(t->vt);
	t->status_rows = st && st[0] ? 1 : 0;
	int gw, gh, cols, rows;
	bool glyphs = dmc_vt_glyph_cell(t->vt, &gw, &gh) != 0;
	int cw = t->fcw, ch = t->fch;
	if (glyphs && rc > 0 && rr > 0) {
		/* whole multiples of the machine's pixels where they fit */
		int sx = W / (rc * gw), sy = H / ((rr + t->status_rows) * gh);
		int s = sx < sy ? sx : sy;
		if (s >= 1) {
			cw = gw * s;
			ch = gh * s;
		} else {
			cw = W / rc;
			ch = H / (rr + t->status_rows);
		}
	} else {
		if (rc > 0)
			cw = W / rc;
		if (rr > 0)
			ch = H / (rr + t->status_rows);
	}
	if (cw < 2)
		cw = 2;
	if (ch < 2)
		ch = 2;
	cols = rc > 0 ? rc : W / cw;
	rows = rr > 0 ? rr : H / ch - t->status_rows;
	if (cols < 10)
		cols = 10;
	if (rows < 3)
		rows = 3;
	if (cols > MAX_COLS)
		cols = MAX_COLS;
	if (rows > 200)
		rows = 200;
	t->cw = cw;
	t->ch = ch;
	t->ox = rc > 0 ? (W - cols * cw) / 2 : 0;
	t->oy = rr > 0 ? (H - (rows + t->status_rows) * ch) / 2 : 0;
	if (t->ox < 0)
		t->ox = 0;
	if (t->oy < 0)
		t->oy = 0;
	dmc_vt_set_cell_pixels(t->vt, cw, ch);
	if (cols != dmc_vt_cols(t->vt) || rows != dmc_vt_rows(t->vt)) {
		t->sel = t->selecting = false;
		dmc_vt_resize(t->vt, cols, rows);
		update_scrollbar(t);
	}
}

/* ------------------------------------------------------------------ */
/* drawing a row */

static bool row_buffer(TermView *t, HDC dc, int w, int h)
{
	if (t->row_dc && t->row_w >= w && t->row_h >= h)
		return true;
	if (t->row_dc) {
		SelectObject(t->row_dc, t->row_old);
		DeleteObject(t->row_bmp);
		DeleteDC(t->row_dc);
		t->row_dc = NULL;
	}
	BITMAPINFO bi;
	memset(&bi, 0, sizeof(bi));
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h;     /* top down */
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	void *bits = NULL;
	HBITMAP b = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
	if (!b || !bits)
		return false;
	t->row_dc = CreateCompatibleDC(dc);
	t->row_bmp = b;
	t->row_old = SelectObject(t->row_dc, b);
	t->row_px = (DWORD *)bits;
	t->row_w = w;
	t->row_h = h;
	SetBkMode(t->row_dc, OPAQUE);
	return true;
}

static void px_rect(TermView *t, int x0, int y0, int x1, int y1,
		    unsigned long c)
{
	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 > t->row_w)
		x1 = t->row_w;
	if (y1 > t->row_h)
		y1 = t->row_h;
	for (int y = y0; y < y1; y++) {
		DWORD *p = t->row_px + (size_t)y * t->row_w;
		for (int x = x0; x < x1; x++)
			p[x] = c & 0xffffff;
	}
}

/* a machine font's glyph, stretched over its cell */
static void draw_glyph(TermView *t, uint32_t cp, int x0, int w,
		       unsigned long fg)
{
	int gw, gh, ch = t->ch;
	if (!dmc_vt_glyph(t->vt, cp, &gw, &gh) || gw <= 0 || gh <= 0)
		return;
	for (int y = 0; y < ch; y++) {
		uint32_t bits = dmc_vt_glyph_row(t->vt, cp, y * gh / ch);
		if (!bits)
			continue;
		DWORD *p = t->row_px + (size_t)y * t->row_w;
		for (int x = 0; x < w; x++) {
			int gx = x * gw / w;
			if ((bits >> (gw - 1 - gx)) & 1)
				p[x0 + x] = fg & 0xffffff;
		}
	}
}

/* a picture's tile over its cell, its transparent pixels showing the
   text under it */
static void draw_tile(TermView *t, int vrow, int col, int x0)
{
	dmc_image im;
	const uint32_t *px = dmc_vt_cell_image(t->vt, vrow, col, &im);
	if (!px || im.cell_width <= 0 || im.cell_height <= 0)
		return;
	int cw = t->cw, ch = t->ch;
	for (int y = 0; y < ch; y++) {
		int sy = im.ty * im.cell_height + y * im.cell_height / ch;
		if (sy < 0 || sy >= im.height)
			continue;
		DWORD *d = t->row_px + (size_t)y * t->row_w + x0;
		const uint32_t *s = px + (size_t)sy * im.width;
		for (int x = 0; x < cw; x++) {
			int sx = im.tx * im.cell_width + x * im.cell_width / cw;
			if (sx < 0 || sx >= im.width)
				continue;
			uint32_t v = s[sx];
			unsigned a = v >> 24;
			if (a == 255) {
				d[x] = v & 0xffffff;
			} else if (a) {
				DWORD o = d[x];
				unsigned r = (((v >> 16) & 255) * a + ((o >> 16) & 255) * (255 - a)) / 255;
				unsigned g = (((v >> 8) & 255) * a + ((o >> 8) & 255) * (255 - a)) / 255;
				unsigned b = ((v & 255) * a + (o & 255) * (255 - a)) / 255;
				d[x] = (r << 16) | (g << 8) | b;
			}
		}
	}
}

/* a UTF-8 string as UTF-16 (surrogates for what's past the BMP) */
static int utf16(const char *s, WCHAR *out, int max)
{
	int n = 0;
	const unsigned char *p = (const unsigned char *)s;
	while (*p && n < max - 1) {
		unsigned long c = *p++;
		int more = 0;
		if (c >= 0xf0) {
			c &= 7;
			more = 3;
		} else if (c >= 0xe0) {
			c &= 15;
			more = 2;
		} else if (c >= 0xc0) {
			c &= 31;
			more = 1;
		}
		while (more-- > 0 && (*p & 0xc0) == 0x80)
			c = (c << 6) | (*p++ & 0x3f);
		if (c >= 0x10000) {
			c -= 0x10000;
			out[n++] = (WCHAR)(0xd800 + (c >> 10));
			out[n++] = (WCHAR)(0xdc00 + (c & 0x3ff));
		} else {
			out[n++] = (WCHAR)c;
		}
	}
	return n;
}

static int text_flags(int f)
{
	return f & (DMC_CELL_BOLD | DMC_CELL_ITALIC | DMC_CELL_INVISIBLE |
		    DMC_CELL_BLINK | DMC_CELL_GLYPH | DMC_CELL_WIDE |
		    DMC_CELL_COMBINING);
}

/* the cells of view row vrow into the row buffer: its backgrounds, text,
   glyphs, lines, pictures and the cursor; ncols cells (a double width
   line's half) */
static void draw_row(TermView *t, int vrow, int ncols, bool cursor_here)
{
	dmc_vt *vt = t->vt;
	int cw = t->cw, ch = t->ch;
	HDC dc = t->row_dc;
	dmc_cell cells[MAX_COLS];
	if (ncols > MAX_COLS)
		ncols = MAX_COLS;
	bool any_glyph = false, any_image = false, any_lines = false;
	for (int x = 0; x < ncols; x++) {
		dmc_vt_cell(vt, vrow, x, &cells[x]);
		if (cells[x].flags & DMC_CELL_BLINK)
			t->has_blink = true;
		if (cells[x].flags & DMC_CELL_GLYPH)
			any_glyph = true;
		if (cells[x].flags & DMC_CELL_IMAGE)
			any_image = true;
		if (cells[x].flags & (DMC_CELL_UNDERLINE | DMC_CELL_DOUBLE_UNDERLINE |
				      DMC_CELL_CURLY_UNDERLINE | DMC_CELL_STRIKE |
				      DMC_CELL_OVERLINE | DMC_CELL_LINK))
			any_lines = true;
	}
	/* the backgrounds and the font's text, in runs alike */
	WCHAR text[MAX_COLS * 2 + 8];
	INT dx[MAX_COLS * 2 + 8];
	int x = 0;
	while (x < ncols) {
		const dmc_cell &c = cells[x];
		int f = text_flags(c.flags);
		bool hidden = (f & DMC_CELL_INVISIBLE) ||
			((f & DMC_CELL_BLINK) && t->blink_off) ||
			(f & DMC_CELL_GLYPH);
		int n = 0, cells_n = 0;
		if (c.flags & DMC_CELL_WIDE_TAIL) {
			text[0] = ' ';
			dx[0] = cw;
			n = cells_n = 1;
		} else if (f & (DMC_CELL_WIDE | DMC_CELL_COMBINING)) {
			/* one cell (or two) of its own */
			cells_n = (f & DMC_CELL_WIDE) && x + 1 < ncols ? 2 : 1;
			if (hidden) {
				text[0] = ' ';
				n = 1;
			} else {
				n = utf16(dmc_vt_cell_text(vt, vrow, x), text, 8);
				if (!n) {
					text[0] = ' ';
					n = 1;
				}
			}
			for (int i = 0; i < n; i++)
				dx[i] = i == 0 ? cw * cells_n : 0;
		} else {
			while (x + cells_n < ncols && n < MAX_COLS * 2) {
				const dmc_cell &d = cells[x + cells_n];
				if (d.fg != c.fg || d.bg != c.bg ||
				    text_flags(d.flags) != f ||
				    (d.flags & DMC_CELL_WIDE_TAIL))
					break;
				uint32_t cp = hidden || d.ch < 32 ? ' ' : d.ch;
				if (cp >= 0x10000) {
					cp -= 0x10000;
					text[n] = (WCHAR)(0xd800 + (cp >> 10));
					dx[n++] = cw;
					text[n] = (WCHAR)(0xdc00 + (cp & 0x3ff));
					dx[n++] = 0;
				} else {
					text[n] = (WCHAR)cp;
					dx[n++] = cw;
				}
				cells_n++;
			}
		}
		SelectObject(dc, t->fonts[((f & DMC_CELL_BOLD) ? 1 : 0) |
					  ((f & DMC_CELL_ITALIC) ? 2 : 0)]);
		SetTextColor(dc, cref(c.fg));
		SetBkColor(dc, cref(c.bg));
		RECT cell = { x * cw, 0, (x + cells_n) * cw, ch };
		/* (the font's baseline in the cell's middle when it's taller) */
		int ty = (ch - t->fch) / 2;
		ExtTextOutW(dc, x * cw, ty > 0 ? ty : 0, ETO_OPAQUE | ETO_CLIPPED,
			    &cell, text, n, dx);
		x += cells_n;
	}
	GdiFlush();
	if (any_glyph)
		for (x = 0; x < ncols; x++) {
			const dmc_cell &c = cells[x];
			if (!(c.flags & DMC_CELL_GLYPH) || (c.flags & DMC_CELL_INVISIBLE) ||
			    ((c.flags & DMC_CELL_BLINK) && t->blink_off))
				continue;
			int w = (c.flags & DMC_CELL_WIDE) ? 2 * cw : cw;
			if (x * cw + w > t->row_w)
				w = t->row_w - x * cw;
			draw_glyph(t, c.ch, x * cw, w, c.fg);
		}
	if (any_lines) {
		int th = ch / 16 > 0 ? ch / 16 : 1;
		for (x = 0; x < ncols; x++) {
			const dmc_cell &c = cells[x];
			int l = x * cw, r = l + cw, f = c.flags;
			if (f & DMC_CELL_INVISIBLE)
				continue;
			if (f & (DMC_CELL_UNDERLINE | DMC_CELL_LINK))
				px_rect(t, l, ch - 2 * th, r, ch - th, c.fg);
			if (f & DMC_CELL_DOUBLE_UNDERLINE) {
				px_rect(t, l, ch - th, r, ch, c.fg);
				px_rect(t, l, ch - 3 * th, r, ch - 2 * th, c.fg);
			}
			if (f & DMC_CELL_CURLY_UNDERLINE)
				for (int i = l; i < r; i++) {
					int y = ch - 2 * th - ((i / (2 * th)) & 1) * th;
					px_rect(t, i, y, i + 1, y + th, c.fg);
				}
			if (f & DMC_CELL_STRIKE)
				px_rect(t, l, ch / 2, r, ch / 2 + th, c.fg);
			if (f & DMC_CELL_OVERLINE)
				px_rect(t, l, 0, r, th, c.fg);
		}
	}
	if (any_image)
		for (x = 0; x < ncols; x++)
			if (cells[x].flags & DMC_CELL_IMAGE)
				draw_tile(t, vrow, x, x * cw);
	if (cursor_here) {
		int cx = dmc_vt_cursor_col(vt);
		if (cx >= 0 && cx < ncols) {
			unsigned long cc = dmc_vt_cursor_color(vt) & 0xffffff;
			unsigned long bg = cells[cx].bg & 0xffffff;
			int l = cx * cw, r = l + ((cells[cx].flags & DMC_CELL_WIDE) ? 2 * cw : cw);
			int style = dmc_vt_cursor_style(vt);
			int th = ch / 8 > 1 ? ch / 8 : 2;
			if (!t->focused) {
				px_rect(t, l, 0, r, 1, cc);
				px_rect(t, l, ch - 1, r, ch, cc);
				px_rect(t, l, 0, l + 1, ch, cc);
				px_rect(t, r - 1, 0, r, ch, cc);
			} else if (style == 3 || style == 4) {
				px_rect(t, l, ch - th, r, ch, cc);
			} else if (style == 5 || style == 6) {
				px_rect(t, l, 0, l + th / 2 + 1, ch, cc);
			} else {
				/* a block: the background the cursor's colour, the
				   text the background */
				for (int y = 0; y < ch && y < t->row_h; y++) {
					DWORD *p = t->row_px + (size_t)y * t->row_w;
					for (int i = l; i < r && i < t->row_w; i++)
						p[i] = (p[i] & 0xffffff) == bg ? cc : bg;
				}
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/* the selection */

/* the selection from its first cell to its last */
static void sel_range(TermView *t, int *r0, int *c0, int *r1, int *c1)
{
	if (t->ar < t->er || (t->ar == t->er && t->ac <= t->ec)) {
		*r0 = t->ar; *c0 = t->ac; *r1 = t->er; *c1 = t->ec;
	} else {
		*r0 = t->er; *c0 = t->ec; *r1 = t->ar; *c1 = t->ac;
	}
}

static void sel_clear(TermView *t)
{
	if (t->sel)
		InvalidateRect(t->w, NULL, FALSE);
	t->sel = t->selecting = false;
}

/* the selection's cells of each row on the screen, inverted */
static void paint_selection(TermView *t, HDC mem)
{
	if (!t->sel)
		return;
	int r0, c0, r1, c1;
	sel_range(t, &r0, &c0, &r1, &c1);
	int rows = dmc_vt_rows(t->vt), cols = dmc_vt_cols(t->vt);
	for (int y = 0; y < rows; y++) {
		int vrow = y + t->scroll;
		if (vrow < r0 || vrow > r1)
			continue;
		int from = vrow == r0 ? c0 : 0, to = vrow == r1 ? c1 : cols - 1;
		int k = dmc_vt_line_size(t->vt, vrow) == DMC_LINE_SINGLE ? 1 : 2;
		int x0 = from * k * t->cw, x1 = (to + 1) * k * t->cw;
		if (x1 > cols * t->cw)
			x1 = cols * t->cw;
		if (x1 > x0)
			PatBlt(mem, t->ox + x0, t->oy + y * t->ch, x1 - x0, t->ch,
			       DSTINVERT);
	}
}

/* the blanks at the ends of the lines dropped, in place */
static void trim_ends(char *s)
{
	char *o = s, *blank = NULL;
	for (const char *p = s; *p; p++) {
		if (*p == ' ') {
			if (!blank)
				blank = o;
		} else if (*p == '\n') {
			if (blank)
				o = blank;
			blank = NULL;
		} else {
			blank = NULL;
		}
		*o++ = *p;
	}
	if (blank)
		o = blank;
	*o = '\0';
}

/* UTF-8 to UTF-16 (malloc'ed, CR LF for each line end) */
static WCHAR *wide_lines(const char *s, int *len)
{
	int n = 0;
	for (const char *p = s; *p; p++)
		n += *p == '\n' ? 2 : 1;
	WCHAR *w = (WCHAR *)malloc(((size_t)n + 1) * sizeof(WCHAR));
	if (!w)
		return NULL;
	int k = 0;
	const unsigned char *p = (const unsigned char *)s;
	while (*p) {
		unsigned long c = *p++;
		int more = 0;
		if (c >= 0xf0) {
			c &= 7;
			more = 3;
		} else if (c >= 0xe0) {
			c &= 15;
			more = 2;
		} else if (c >= 0xc0) {
			c &= 31;
			more = 1;
		}
		while (more-- > 0 && (*p & 0xc0) == 0x80)
			c = (c << 6) | (*p++ & 0x3f);
		if (c == '\n')
			w[k++] = '\r';
		if (c >= 0x10000) {
			c -= 0x10000;
			w[k++] = (WCHAR)(0xd800 + (c >> 10));
			w[k++] = (WCHAR)(0xdc00 + (c & 0x3ff));
		} else {
			w[k++] = (WCHAR)c;
		}
	}
	w[k] = 0;
	if (len)
		*len = k;
	return w;
}

static HGLOBAL global_copy(const void *data, size_t n)
{
	HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, n);
	if (!g)
		return NULL;
	void *p = GlobalLock(g);
	memcpy(p, data, n);
	GlobalUnlock(g);
	return g;
}

/* the selection's text to the clipboard: Unicode, and the ANSI code
   page's for Windows 9x programs */
static bool copy_selection(TermView *t)
{
	if (!t->sel)
		return false;
	int r0, c0, r1, c1;
	sel_range(t, &r0, &c0, &r1, &c1);
	const char *r = dmc_vt_text_range(t->vt, r0, c0, r1, c1);
	char *text = (char *)malloc(strlen(r) + 1);
	if (!text)
		return false;
	strcpy(text, r);
	trim_ends(text);
	int len = 0;
	WCHAR *w = wide_lines(text, &len);
	free(text);
	if (!w)
		return false;
	bool ok = false;
	if (OpenClipboard(t->w)) {
		EmptyClipboard();
		HGLOBAL g = global_copy(w, ((size_t)len + 1) * sizeof(WCHAR));
		if (g && SetClipboardData(CF_UNICODETEXT, g))
			ok = true;
		int an = WideCharToMultiByte(CP_ACP, 0, w, len + 1, NULL, 0,
					     NULL, NULL);
		char *a = an > 0 ? (char *)malloc((size_t)an) : NULL;
		if (a && WideCharToMultiByte(CP_ACP, 0, w, len + 1, a, an, NULL,
					     NULL) > 0) {
			g = global_copy(a, (size_t)an);
			if (g && SetClipboardData(CF_TEXT, g))
				ok = true;
		}
		free(a);
		CloseClipboard();
	}
	free(w);
	return ok;
}

static void select_all(TermView *t)
{
	t->ar = -dmc_vt_history(t->vt);
	t->ac = 0;
	t->er = dmc_vt_rows(t->vt) - 1;
	t->ec = dmc_vt_cols(t->vt) - 1;
	t->sel = true;
	t->selecting = false;
	InvalidateRect(t->w, NULL, FALSE);
}

/* the cell under a point as a row of dmc_vt_cell(), in the screen */
static void sel_cell(TermView *t, int x, int y, int *row, int *col)
{
	int rows = dmc_vt_rows(t->vt), cols = dmc_vt_cols(t->vt);
	int r = (y - t->oy) / (t->ch > 0 ? t->ch : 1);
	if (y < t->oy)
		r = 0;
	if (r >= rows)
		r = rows - 1;
	*row = r + t->scroll;
	int k = dmc_vt_line_size(t->vt, *row) == DMC_LINE_SINGLE ? 1 : 2;
	int c = (x - t->ox) / (t->cw * k > 0 ? t->cw * k : 1);
	if (x < t->ox)
		c = 0;
	if (c >= cols)
		c = cols - 1;
	*col = c;
}

/* the word (no blanks) under a point */
static void select_word(TermView *t, int x, int y)
{
	int row, col, cols = dmc_vt_cols(t->vt);
	sel_cell(t, x, y, &row, &col);
	uint32_t c = dmc_vt_char(t->vt, row, col);
	if (c == ' ' || c == 0) {
		sel_clear(t);
		return;
	}
	int a = col, b = col;
	while (a > 0 && dmc_vt_char(t->vt, row, a - 1) != ' ')
		a--;
	while (b < cols - 1 && dmc_vt_char(t->vt, row, b + 1) != ' ')
		b++;
	t->ar = t->er = row;
	t->ac = a;
	t->ec = b;
	t->sel = true;
	InvalidateRect(t->w, NULL, FALSE);
}

static void paste(TermView *t);

/* the right button's menu, as HyperTerminal has it */
static void context_menu(TermView *t, LPARAM lp)
{
	enum { CM_COPY = 1, CM_PASTE, CM_ALL };
	HMENU m = CreatePopupMenu();
	AppendMenu(m, MF_STRING | (t->sel ? 0 : MF_GRAYED), CM_COPY,
		   "&Copy\tCtrl+Insert");
	AppendMenu(m, MF_STRING |
		   (IsClipboardFormatAvailable(CF_TEXT) ||
		    IsClipboardFormatAvailable(CF_UNICODETEXT) ? 0 : MF_GRAYED),
		   CM_PASTE, "&Paste to Host\tShift+Insert");
	AppendMenu(m, MF_SEPARATOR, 0, NULL);
	AppendMenu(m, MF_STRING, CM_ALL, "Select &All");
	POINT p;
	p.x = (short)LOWORD(lp);
	p.y = (short)HIWORD(lp);
	ClientToScreen(t->w, &p);
	int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON,
				      p.x, p.y, 0, t->w, NULL);
	DestroyMenu(m);
	switch (cmd) {
	case CM_COPY:
		copy_selection(t);
		sel_clear(t);
		break;
	case CM_PASTE:
		paste(t);
		break;
	case CM_ALL:
		select_all(t);
		break;
	}
}

static void paint(TermView *t, HDC dc, RECT *area)
{
	RECT r;
	GetClientRect(t->w, &r);
	HDC mem = CreateCompatibleDC(dc);
	HBITMAP bmp = CreateCompatibleBitmap(dc, r.right, r.bottom);
	HGDIOBJ oldb = SelectObject(mem, bmp);
	HBRUSH bg = CreateSolidBrush(cref(dmc_vt_background(t->vt)));
	FillRect(mem, &r, bg);
	DeleteObject(bg);
	dmc_vt *vt = t->vt;
	int rows = dmc_vt_rows(vt), cols = dmc_vt_cols(vt);
	int cw = t->cw, ch = t->ch, w = cols * cw;
	t->has_blink = false;
	if (row_buffer(t, dc, w, ch)) {
		HGDIOBJ oldf = SelectObject(t->row_dc, app.mono_font);
		for (int y = 0; y < rows; y++) {
			int vrow = y + t->scroll;
			int size = dmc_vt_line_size(vt, vrow);
			bool cur = t->scroll == 0 && dmc_vt_cursor_visible(vt) &&
				dmc_vt_cursor_row(vt) == y;
			int dy = t->oy + y * ch;
			if (size == DMC_LINE_SINGLE) {
				draw_row(t, vrow, cols, cur);
				BitBlt(mem, t->ox, dy, w, ch, t->row_dc, 0, 0, SRCCOPY);
				continue;
			}
			/* half the cells, twice as wide (and for double height
			   the top or bottom half, twice as tall) */
			draw_row(t, vrow, (cols + 1) / 2, cur);
			int hw = (cols + 1) / 2 * cw;
			SetStretchBltMode(mem, COLORONCOLOR);
			if (size == DMC_LINE_DOUBLE_WIDTH)
				StretchBlt(mem, t->ox, dy, w, ch, t->row_dc, 0, 0,
					   hw, ch, SRCCOPY);
			else
				StretchBlt(mem, t->ox, dy, w, ch, t->row_dc, 0,
					   size == DMC_LINE_DOUBLE_TOP ? 0 : ch / 2,
					   hw, ch / 2, SRCCOPY);
		}
		paint_selection(t, mem);
		/* the status line under the screen */
		if (t->status_rows) {
			const char *s = dmc_vt_status_line(vt);
			WCHAR text[MAX_COLS + 8];
			INT dx[MAX_COLS + 8];
			int n = utf16(s, text, cols < MAX_COLS ? cols : MAX_COLS);
			for (int i = 0; i < n; i++)
				dx[i] = cw;
			RECT sr = { 0, 0, w, ch };
			SelectObject(t->row_dc, app.mono_font);
			SetTextColor(t->row_dc, cref(dmc_vt_background(vt)));
			SetBkColor(t->row_dc, cref(dmc_vt_foreground(vt)));
			ExtTextOutW(t->row_dc, 0, 0, ETO_OPAQUE | ETO_CLIPPED, &sr,
				    text, n, dx);
			BitBlt(mem, t->ox, t->oy + rows * ch, w, ch, t->row_dc, 0,
			       0, SRCCOPY);
		}
		SelectObject(t->row_dc, oldf);
	}
	BitBlt(dc, area->left, area->top, area->right - area->left,
	       area->bottom - area->top, mem, area->left, area->top, SRCCOPY);
	SelectObject(mem, oldb);
	DeleteObject(bmp);
	DeleteDC(mem);
}

/* ------------------------------------------------------------------ */
/* what the user does */

static void send_text(TermView *t, const char *s, int n)
{
	if (t->fn && n > 0)
		t->fn(s, n, t->ctx);
}

static void to_bottom(TermView *t)
{
	if (t->scroll) {
		t->scroll = 0;
		update_scrollbar(t);
		InvalidateRect(t->w, NULL, FALSE);
	}
}

static int mods_now()
{
	int m = 0;
	if (GetKeyState(VK_SHIFT) < 0)
		m |= DMC_MOD_SHIFT;
	if (GetKeyState(VK_CONTROL) < 0)
		m |= DMC_MOD_CTRL;
	if (GetKeyState(VK_MENU) < 0)
		m |= DMC_MOD_ALT;
	return m;
}

/* what is typed, on the screen too when the echo is on (not a key's
   escape sequence: an arrow key does not move the cursor here) */
static void echo_typed(TermView *t, const char *b, int n)
{
	if (t->echo && n > 0 && b[0] != 0x1b)
		termview_feed(t->w, b, n);
}

static void send_key(TermView *t, int key, int mods)
{
	char b[64];
	int n = dmc_vt_key_mods(t->vt, key, mods, b, sizeof(b));
	to_bottom(t);
	send_text(t, b, n);
	echo_typed(t, b, n);
}

/* a character typed, as the terminal sends it */
static void send_cp(TermView *t, unsigned long cp, int mods)
{
	char b[64];
	int n = dmc_vt_char_bytes(t->vt, (uint32_t)cp, mods, b, sizeof(b));
	send_text(t, b, n);
	echo_typed(t, b, n);
}

/* a character in the ANSI code page as Unicode */
static unsigned long from_ansi(unsigned char c)
{
	if (c < 0x80)
		return c;
	WCHAR w;
	char mb = (char)c;
	if (MultiByteToWideChar(CP_ACP, 0, &mb, 1, &w, 1) != 1)
		return 0;
	return w;
}

static void paste(TermView *t)
{
	if (!OpenClipboard(t->w))
		return;
	bool bracket = dmc_vt_bracketed_paste(t->vt) != 0;
	if (bracket)
		send_text(t, "\x1b[200~", 6);
	HANDLE h = GetClipboardData(CF_UNICODETEXT);
	const WCHAR *ws = h ? (const WCHAR *)GlobalLock(h) : NULL;
	if (ws) {
		for (const WCHAR *p = ws; *p; p++) {
			if (*p == '\n' && p > ws && p[-1] == '\r')
				continue;
			unsigned long c = *p;
			if (c >= 0xd800 && c < 0xdc00 && p[1] >= 0xdc00 &&
			    p[1] < 0xe000) {
				c = 0x10000 + ((c - 0xd800) << 10) + (p[1] - 0xdc00);
				p++;
			}
			send_cp(t, c == '\n' ? '\r' : c, 0);
		}
		GlobalUnlock(h);
	} else if ((h = GetClipboardData(CF_TEXT)) != NULL) {
		const char *s = (const char *)GlobalLock(h);
		if (s) {
			for (const char *p = s; *p; p++) {
				if (*p == '\n' && p > s && p[-1] == '\r')
					continue;
				send_cp(t, *p == '\n' ? '\r' : from_ansi((unsigned char)*p), 0);
			}
			GlobalUnlock(h);
		}
	}
	if (bracket)
		send_text(t, "\x1b[201~", 6);
	CloseClipboard();
}

/* the cell under a point (in the screen's columns and rows) */
static void cell_at(TermView *t, LPARAM lp, int *col, int *row)
{
	int x = (short)LOWORD(lp) - t->ox, y = (short)HIWORD(lp) - t->oy;
	*col = x < 0 ? 0 : x / t->cw;
	*row = y < 0 ? 0 : y / t->ch;
}

static bool mouse_report(TermView *t, int button, LPARAM lp, int press,
			 int motion)
{
	if (!dmc_vt_mouse_wanted(t->vt))
		return false;
	int col, row;
	cell_at(t, lp, &col, &row);
	char b[64];
	int n = dmc_vt_mouse(t->vt, button, col, row, press, motion, mods_now(),
			     b, sizeof(b));
	send_text(t, b, n);
	return true;
}

static int vk_key(WPARAM wp, int mods, TermView *t, HWND w)
{
	switch (wp) {
	case VK_UP: return DMC_KEY_UP;
	case VK_DOWN: return DMC_KEY_DOWN;
	case VK_LEFT: return DMC_KEY_LEFT;
	case VK_RIGHT: return DMC_KEY_RIGHT;
	case VK_HOME: return DMC_KEY_HOME;
	case VK_END: return DMC_KEY_END;
	case VK_PRIOR:
		if (mods == DMC_MOD_SHIFT) {
			SendMessage(w, WM_VSCROLL, SB_PAGEUP, 0);
			return -1;
		}
		return DMC_KEY_PGUP;
	case VK_NEXT:
		if (mods == DMC_MOD_SHIFT) {
			SendMessage(w, WM_VSCROLL, SB_PAGEDOWN, 0);
			return -1;
		}
		return DMC_KEY_PGDN;
	case VK_INSERT:
		if (mods == DMC_MOD_SHIFT) {
			paste(t);
			return -1;
		}
		if (mods == DMC_MOD_CTRL) {
			copy_selection(t);
			return -1;
		}
		return DMC_KEY_INSERT;
	case VK_DELETE: return DMC_KEY_DELETE;
	case VK_TAB: return mods & DMC_MOD_SHIFT ? DMC_KEY_BACKTAB : 0;
	case VK_F11: return DMC_KEY_F11;
	case VK_F12: return DMC_KEY_F12;
	}
	if (wp >= VK_F1 && wp <= VK_F10)
		return DMC_KEY_F1 + (int)(wp - VK_F1);
	if (wp >= VK_F13 && wp <= VK_F20)
		return DMC_KEY_F13 + (int)(wp - VK_F13);
	return 0;
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
	case WM_KILLFOCUS: {
		t->focused = m == WM_SETFOCUS;
		char b[16];
		send_text(t, b, dmc_vt_focus(t->vt, t->focused, b, sizeof(b)));
		InvalidateRect(w, NULL, FALSE);
		return 0;
	}
	case WM_LBUTTONDOWN:
	case WM_LBUTTONDBLCLK:
	case WM_MBUTTONDOWN: {
		SetFocus(w);
		SetCapture(w);
		int button = m == WM_MBUTTONDOWN ? 1 : 0;
		t->buttons |= 1 << button;
		/* (Shift: a selection, also when the remote takes the mouse) */
		bool shift = (wp & MK_SHIFT) != 0;
		if (!shift && mouse_report(t, button, lp, 1, 0))
			return 0;
		if (button)
			return 0;
		int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
		if (m == WM_LBUTTONDBLCLK) {
			select_word(t, x, y);
			return 0;
		}
		sel_clear(t);
		sel_cell(t, x, y, &t->ar, &t->ac);
		t->er = t->ar;
		t->ec = t->ac;
		t->selecting = true;
		if (!shift) {
			/* a RIPscrip button's */
			int cols = dmc_vt_cols(t->vt), rows = dmc_vt_rows(t->vt);
			double fx = (x - t->ox) / (double)(cols * t->cw);
			double fy = (y - t->oy) / (double)(rows * t->ch);
			char b[256];
			int n = dmc_vt_tap(t->vt, fx, fy, b, sizeof(b));
			if (n > 0) {
				t->selecting = false;
				to_bottom(t);
				send_text(t, b, n);
			}
		}
		return 0;
	}
	case WM_LBUTTONUP:
	case WM_MBUTTONUP: {
		int button = m == WM_LBUTTONUP ? 0 : 1;
		t->buttons &= ~(1 << button);
		if (!t->buttons)
			ReleaseCapture();
		if (!button && t->selecting) {
			t->selecting = false;
			return 0;
		}
		mouse_report(t, button, lp, 0, 0);
		return 0;
	}
	case WM_RBUTTONDOWN:
		if (!(wp & MK_SHIFT) && mouse_report(t, 2, lp, 1, 0)) {
			t->buttons |= 4;
			return 0;
		}
		return 0;
	case WM_RBUTTONUP:
		if (t->buttons & 4) {
			t->buttons &= ~4;
			mouse_report(t, 2, lp, 0, 0);
			return 0;
		}
		context_menu(t, lp);
		return 0;
	case WM_MOUSEMOVE: {
		if (t->selecting) {
			int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
			RECT r;
			GetClientRect(w, &r);
			/* above or under the screen: the scrollback scrolled */
			if (y < 0)
				SendMessage(w, WM_VSCROLL, SB_LINEUP, 0);
			else if (y >= r.bottom)
				SendMessage(w, WM_VSCROLL, SB_LINEDOWN, 0);
			int row, col;
			sel_cell(t, x, y, &row, &col);
			if (row != t->er || col != t->ec || !t->sel) {
				t->er = row;
				t->ec = col;
				t->sel = t->er != t->ar || t->ec != t->ac || t->sel;
				InvalidateRect(w, NULL, FALSE);
			}
			return 0;
		}
		int b = t->buttons & 1 ? 0 : t->buttons & 2 ? 1 : t->buttons & 4 ? 2 : 3;
		mouse_report(t, b, lp, 1, 1);
		return 0;
	}
	case WM_MOUSEWHEEL: {
		int d = GET_WHEEL_DELTA_WPARAM(wp);
		POINT p;
		p.x = (short)LOWORD(lp);
		p.y = (short)HIWORD(lp);
		ScreenToClient(w, &p);
		if (mouse_report(t, d > 0 ? 64 : 65, MAKELPARAM(p.x, p.y), 1, 0))
			return 0;
		for (int i = 0; i < 3; i++)
			SendMessage(w, WM_VSCROLL, d > 0 ? SB_LINEUP : SB_LINEDOWN, 0);
		return 0;
	}
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(w, &ps);
		paint(t, dc, &ps.rcPaint);
		EndPaint(w, &ps);
		return 0;
	}
	case WM_TIMER:
		if (wp == TIMER_BLINK) {
			t->blink_off = !t->blink_off;
			if (t->has_blink)
				InvalidateRect(w, NULL, FALSE);
			return 0;
		}
		break;
	case WM_SIZE:
		layout(t);
		InvalidateRect(w, NULL, FALSE);
		return 0;
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
		InvalidateRect(w, NULL, FALSE);
		return 0;
	}
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN: {
		int mods = mods_now();
		int key = vk_key(wp, mods, t, w);
		if (key < 0)
			return 0;
		/* Ctrl and a digit: a Commodore's colours, the VT's controls */
		if (!key && (mods & DMC_MOD_CTRL) && !(mods & DMC_MOD_ALT) &&
		    wp >= '0' && wp <= '9') {
			to_bottom(t);
			send_cp(t, (unsigned long)wp, DMC_MOD_CTRL);
			return 0;
		}
		if (!key)
			break;
		send_key(t, key, mods);
		return 0;
	}
	case WM_SYSCHAR:
		/* Alt and a character: ESC and it, as the terminal sends them */
		if (wp >= 0x20) {
			to_bottom(t);
			send_cp(t, from_ansi((unsigned char)wp), DMC_MOD_ALT);
			return 0;
		}
		break;
	case WM_CHAR: {
		int mods = mods_now() & ~DMC_MOD_ALT;
		to_bottom(t);
		switch (wp) {
		case '\r':
			send_key(t, DMC_KEY_ENTER, mods);
			return 0;
		case 0x08:
			send_key(t, DMC_KEY_BACKSPACE, mods);
			return 0;
		case '\t':
			if (!(mods & DMC_MOD_SHIFT))
				send_key(t, DMC_KEY_TAB, mods);
			return 0;
		case 0x1b:
			if (!(mods & DMC_MOD_CTRL)) {
				send_key(t, DMC_KEY_ESCAPE, mods);
				return 0;
			}
			break;
		}
		if (wp < 0x20 || wp == 0x7f) {
			/* a control the keyboard made (Ctrl and a letter) */
			send_cp(t, wp < 0x20 ? 0x40 + (unsigned long)wp : '?',
				DMC_MOD_CTRL);
			return 0;
		}
		unsigned long c = from_ansi((unsigned char)wp);
		if (c)
			send_cp(t, c, mods & DMC_MOD_SHIFT);
		return 0;
	}
	case WM_NCDESTROY:
		KillTimer(w, TIMER_BLINK);
		SetWindowLongPtr(w, GWLP_USERDATA, 0);
		if (t->row_dc) {
			SelectObject(t->row_dc, t->row_old);
			DeleteObject(t->row_bmp);
			DeleteDC(t->row_dc);
		}
		for (int i = 1; i < 4; i++)
			if (t->fonts[i] != app.mono_font)
				DeleteObject(t->fonts[i]);
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
		wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
		wc.lpfnWndProc = view_proc;
		wc.hInstance = app.inst;
		wc.hCursor = LoadCursor(NULL, IDC_IBEAM);
		wc.lpszClassName = "DModemTermView";
		RegisterClass(&wc);
		registered = true;
	}
	TermView *t = (TermView *)calloc(1, sizeof(TermView));
	t->vt = dmc_vt_new(80, 24, 2000);
	t->fcw = t->cw = 8;
	t->fch = t->ch = 16;
	t->fn = fn;
	t->ctx = ctx;
	dmc_vt_set_backspace(t->vt, 0x7f);
	/* the brand's background and text (D-Modem/brand: docs/palette.md) */
	dmc_vt_set_theme(t->vt, 0xe8f0ff, 0x0a0f1c, 0xe8f0ff, NULL);
	HWND w = make_child("DModemTermView", "", WS_TABSTOP | WS_VSCROLL,
			    parent, id, WS_EX_CLIENTEDGE);
	t->w = w;
	SetWindowLongPtr(w, GWLP_USERDATA, (LONG_PTR)t);
	make_fonts(t);
	measure(t);
	SetTimer(w, TIMER_BLINK, 500, NULL);
	return w;
}

void termview_feed(HWND w, const char *data, int n)
{
	TermView *t = of(w);
	if (!t || n <= 0)
		return;
	int before = dmc_vt_history(t->vt);
	dmc_vt_feed(t->vt, data, n);
	int moved = dmc_vt_history(t->vt) - before;
	if (t->scroll)
		t->scroll -= moved;
	/* the selection stays on its text as the lines go up */
	if (t->sel || t->selecting) {
		t->ar -= moved;
		t->er -= moved;
		if (t->ar < -dmc_vt_history(t->vt) || t->er < -dmc_vt_history(t->vt))
			sel_clear(t);
	}
	if (t->scroll < -dmc_vt_history(t->vt))
		t->scroll = -dmc_vt_history(t->vt);
	char r[256];
	int k;
	while ((k = dmc_vt_reply(t->vt, r, sizeof(r))) > 0)
		send_text(t, r, k);
	/* what went to the printer: to the page's printer */
	while ((k = dmc_vt_printed(t->vt, r, sizeof(r))) > 0)
		if (t->pfn)
			t->pfn(r, k, t->pctx);
	if (dmc_vt_bells(t->vt))
		MessageBeep(MB_OK);
	play_music(t->vt);
	layout(t);
	update_scrollbar(t);
	if (dmc_vt_changed(t->vt))
		InvalidateRect(w, NULL, FALSE);
}

void termview_reset(HWND w)
{
	TermView *t = of(w);
	if (!t)
		return;
	dmc_vt_reset(t->vt);
	t->scroll = 0;
	sel_clear(t);
	layout(t);
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
		dmc_vt_set_backspace(t->vt, (unsigned char)b);
}

void termview_set_ascii(HWND w, int flags, int enter, bool echo)
{
	TermView *t = of(w);
	if (!t)
		return;
	dmc_vt_set_ascii(t->vt, flags);
	dmc_vt_set_enter(t->vt, enter);
	t->echo = echo;
}

void termview_set_profile(HWND w, const char *id)
{
	TermView *t = of(w);
	if (!t || !dmc_vt_set_profile(t->vt, id))
		return;
	t->scroll = 0;
	layout(t);
	update_scrollbar(t);
	InvalidateRect(w, NULL, FALSE);
}

const char *termview_profile(HWND w)
{
	TermView *t = of(w);
	return t ? dmc_vt_get_profile(t->vt) : "xterm";
}

void term_types_fill(HWND combo, const char *id)
{
	int sel = 0;
	SendMessage(combo, CB_RESETCONTENT, 0, 0);
	for (int i = 0; i < dmc_vt_profile_count(); i++) {
		char a[128];
		to_ansi(dmc_vt_profile_label(i), a, sizeof(a));
		SendMessage(combo, CB_ADDSTRING, 0, (LPARAM)a);
		if (id && !strcmp(id, dmc_vt_profile_id(i)))
			sel = i;
	}
	SendMessage(combo, CB_SETCURSEL, (WPARAM)sel, 0);
}

const char *term_types_chosen(HWND combo)
{
	int i = (int)SendMessage(combo, CB_GETCURSEL, 0, 0);
	const char *id = dmc_vt_profile_id(i);
	return id ? id : dmc_vt_profile_id(0);
}

bool termview_has_selection(HWND w)
{
	TermView *t = of(w);
	return t && t->sel;
}

bool termview_copy(HWND w)
{
	TermView *t = of(w);
	return t && copy_selection(t);
}

void termview_paste(HWND w)
{
	TermView *t = of(w);
	if (t)
		paste(t);
}

void termview_select_all(HWND w)
{
	TermView *t = of(w);
	if (t)
		select_all(t);
}

char *termview_text(HWND w, bool selection)
{
	TermView *t = of(w);
	if (!t)
		return NULL;
	const char *s;
	if (selection && t->sel) {
		int r0, c0, r1, c1;
		sel_range(t, &r0, &c0, &r1, &c1);
		s = dmc_vt_text_range(t->vt, r0, c0, r1, c1);
	} else {
		s = dmc_vt_text_range(t->vt, t->scroll, 0,
				      t->scroll + dmc_vt_rows(t->vt) - 1,
				      dmc_vt_cols(t->vt) - 1);
	}
	size_t n = strlen(s);
	char *c = (char *)malloc(n + 1);
	if (c) {
		memcpy(c, s, n + 1);
		trim_ends(c);
	}
	return c;
}

void termview_set_printer(HWND w, TermPrintedFn fn, void *ctx)
{
	TermView *t = of(w);
	if (t) {
		t->pfn = fn;
		t->pctx = ctx;
	}
}
