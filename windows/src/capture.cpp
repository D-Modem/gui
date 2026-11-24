/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * capture.cpp - what comes on a terminal's line kept, as HyperTerminal's
 *               Transfer menu has it: Capture Text (the bytes to a file,
 *               as they come) and Capture to Printer (the text to the
 *               printer when you stop), each with Pause and Resume.  Also
 *               File > Print (the screen or the selection), and the
 *               remote's own printing (its printer port: the VT's media
 *               copy sequences), printed when it stops for some seconds.
 *
 * A capture is of one line (term_line or remote_line, as xfer.cpp has
 * them), and a transfer's bytes are not in it.  The capture folder is
 * kept in dmodem.ini ([capture] folder).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "resource.h"

#include <commdlg.h>

#ifndef OFN_EXPLORER
#define OFN_EXPLORER 0x00080000
#endif

/* the remote's printing is printed once it is quiet for this long */
#define REMOTE_QUIET_MS 3000

/* text for the printer, as UTF-16 */
struct WText {
	WCHAR *p;
	int n, cap;
};

static void wt_add(WText *t, WCHAR c)
{
	if (t->n + 1 >= t->cap) {
		int cap = t->cap ? t->cap * 2 : 4096;
		WCHAR *p = (WCHAR *)realloc(t->p, (size_t)cap * sizeof(WCHAR));
		if (!p)
			return;
		t->p = p;
		t->cap = cap;
	}
	t->p[t->n++] = c;
	t->p[t->n] = 0;
}

static void wt_free(WText *t)
{
	free(t->p);
	t->p = NULL;
	t->n = t->cap = 0;
}

static void wt_add_cp(WText *t, unsigned long c)
{
	if (c >= 0x10000) {
		c -= 0x10000;
		wt_add(t, (WCHAR)(0xd800 + (c >> 10)));
		wt_add(t, (WCHAR)(0xdc00 + (c & 0x3ff)));
	} else {
		wt_add(t, (WCHAR)c);
	}
}

/* UTF-8 text, LF for each line end */
static void wt_add_utf8(WText *t, const char *s, int n)
{
	const unsigned char *p = (const unsigned char *)s, *e = p + n;
	while (p < e) {
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
		while (more-- > 0 && p < e && (*p & 0xc0) == 0x80)
			c = (c << 6) | (*p++ & 0x3f);
		if (c == '\r')
			continue;
		wt_add_cp(t, c);
	}
}

/* the bytes of the line as text: the escape sequences and controls
   dropped (but tabs, line ends and form feeds), UTF-8 where it is
   UTF-8, else the OEM code page (CP437 on a US Windows, as BBSes) */
struct TextFilter {
	int esc;        /* 0 none, 1 after ESC, 2 in CSI, 3 in a string */
	bool str_esc;   /* an ESC in a string (ST next) */
	char csi[16];   /* the CSI's parameters */
	int csin;
	bool printer;   /* in the remote's printer mode (CSI 5 i): the
			   printer's text, which capture_printed() gets */
	unsigned char u[4];
	int un, uwant;  /* a UTF-8 sequence being read */
};

static void oem_char(WText *t, unsigned char b)
{
	WCHAR w;
	char c = (char)b;
	if (MultiByteToWideChar(CP_OEMCP, 0, &c, 1, &w, 1) == 1)
		wt_add(t, w);
	else
		wt_add(t, '?');
}

static void filter_bytes(TextFilter *f, WText *t, const char *data, int n)
{
	for (int i = 0; i < n; i++) {
		unsigned char b = (unsigned char)data[i];
		if (f->uwant) {
			if ((b & 0xc0) == 0x80) {
				f->u[f->un++] = b;
				if (f->un == f->uwant) {
					unsigned long c = f->u[0] &
						(f->uwant == 2 ? 31 : f->uwant == 3 ? 15 : 7);
					for (int k = 1; k < f->un; k++)
						c = (c << 6) | (f->u[k] & 0x3f);
					wt_add_cp(t, c);
					f->uwant = f->un = 0;
				}
				continue;
			}
			/* not UTF-8 after all: the bytes as the OEM's */
			for (int k = 0; k < f->un; k++)
				oem_char(t, f->u[k]);
			f->uwant = f->un = 0;
		}
		if (f->esc == 1) {
			f->csin = 0;
			f->esc = b == '[' ? 2 :
				(b == ']' || b == 'P' || b == '_' || b == '^' ||
				 b == 'X') ? 3 : 0;
			continue;
		}
		if (f->esc == 2) {
			if (b >= 0x40 && b <= 0x7e) {
				f->esc = 0;
				f->csi[f->csin] = '\0';
				if (b == 'i' && !strcmp(f->csi, "5"))
					f->printer = true;
				else if (b == 'i' && !strcmp(f->csi, "4"))
					f->printer = false;
			} else if (f->csin < (int)sizeof(f->csi) - 1) {
				f->csi[f->csin++] = (char)b;
			}
			continue;
		}
		if (f->esc == 3) {
			if (b == 7 || (f->str_esc && b == '\\'))
				f->esc = 0;
			f->str_esc = b == 0x1b;
			continue;
		}
		if (b == 0x1b) {
			f->esc = 1;
			continue;
		}
		if (f->printer)
			continue;
		switch (b) {
		case '\n':
		case '\t':
		case '\f':
			wt_add(t, b);
			continue;
		case 0x08:
			if (t->n > 0 && t->p[t->n - 1] != '\n')
				t->p[--t->n] = 0;
			continue;
		}
		if (b < 0x20 || b == 0x7f)
			continue;
		if (b < 0x80) {
			wt_add(t, b);
		} else if (b >= 0xc2 && b <= 0xf4) {
			f->u[0] = b;
			f->un = 1;
			f->uwant = b >= 0xf0 ? 4 : b >= 0xe0 ? 3 : 2;
		} else {
			oem_char(t, b);
		}
	}
}

/* ------------------------------------------------------------------ */
/* printing */

/* the printer's DC: the print dialog, or the default printer with no
   dialog; *selection: the user chose to print the selection */
static HDC printer_dc(bool ask, bool has_selection, bool *selection)
{
	PRINTDLG pd;
	memset(&pd, 0, sizeof(pd));
	pd.lStructSize = sizeof(pd);
	pd.hwndOwner = app.main;
	pd.Flags = PD_RETURNDC | PD_NOPAGENUMS |
		(has_selection ? PD_SELECTION : PD_NOSELECTION) |
		(ask ? 0 : PD_RETURNDEFAULT);
	pd.nCopies = 1;
	if (!PrintDlg(&pd))
		return NULL;
	if (selection)
		*selection = (pd.Flags & PD_SELECTION) != 0;
	if (pd.hDevMode)
		GlobalFree(pd.hDevMode);
	if (pd.hDevNames)
		GlobalFree(pd.hDevNames);
	return pd.hDC;
}

/* text on pages: a fixed font of 10 points, margins of half an inch,
   long lines wrapped, a form feed a new page; false if it failed */
static bool print_text(HDC dc, const char *title, const WCHAR *text, int n)
{
	int dpix = GetDeviceCaps(dc, LOGPIXELSX), dpiy = GetDeviceCaps(dc, LOGPIXELSY);
	int pw = GetDeviceCaps(dc, HORZRES), ph = GetDeviceCaps(dc, VERTRES);
	if (dpix <= 0 || dpiy <= 0 || pw <= 0 || ph <= 0)
		return false;
	HFONT font = CreateFont(-MulDiv(10, dpiy, 72), 0, 0, 0, FW_NORMAL, 0, 0,
				0, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
				CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
				FIXED_PITCH | FF_MODERN, "Courier New");
	HGDIOBJ old = SelectObject(dc, font);
	TEXTMETRIC tm;
	GetTextMetrics(dc, &tm);
	int lh = tm.tmHeight + tm.tmExternalLeading, cw = tm.tmAveCharWidth;
	if (lh <= 0)
		lh = dpiy / 6;
	if (cw <= 0)
		cw = dpix / 12;
	int mx = dpix / 2, my = dpiy / 2;
	int cols = (pw - 2 * mx) / cw, rows = (ph - 2 * my) / lh;
	if (cols < 20)
		cols = 20;
	if (rows < 5)
		rows = 5;
	DOCINFO di;
	memset(&di, 0, sizeof(di));
	di.cbSize = sizeof(di);
	di.lpszDocName = title;
	bool ok = StartDoc(dc, &di) > 0;
	if (ok)
		ok = StartPage(dc) > 0;
	int row = 0;
	WCHAR line[512];
	int ln = 0, col = 0;
	for (int i = 0; ok && i <= n; i++) {
		WCHAR c = i < n ? text[i] : '\n';
		bool end = c == '\n' || c == '\f' || i == n;
		if (c == '\t') {
			do {
				if (ln < (int)(sizeof(line) / sizeof(line[0])))
					line[ln++] = ' ';
				col++;
			} while (col % 8 && col < cols);
		} else if (!end) {
			if (ln < (int)(sizeof(line) / sizeof(line[0])))
				line[ln++] = c;
			/* (a surrogate pair's first half takes no column) */
			if (c < 0xd800 || c >= 0xdc00)
				col++;
		}
		if (end || col >= cols) {
			if (row >= rows) {
				ok = EndPage(dc) > 0 && StartPage(dc) > 0;
				SelectObject(dc, font);
				row = 0;
			}
			if (ln)
				TextOutW(dc, mx, my + row * lh, line, ln);
			row++;
			ln = col = 0;
			if (c == '\f' && ok) {
				ok = EndPage(dc) > 0 && StartPage(dc) > 0;
				SelectObject(dc, font);
				row = 0;
			}
		}
	}
	if (ok) {
		EndPage(dc);
		EndDoc(dc);
	} else {
		AbortDoc(dc);
	}
	SelectObject(dc, old);
	DeleteObject(font);
	return ok;
}

/* text to the default printer (no dialog) */
static bool print_default(const char *title, const WText *t)
{
	HDC dc = printer_dc(false, false, NULL);
	if (!dc)
		return false;
	bool ok = print_text(dc, title, t->p ? t->p : L"", t->n);
	DeleteDC(dc);
	return ok;
}

/* ------------------------------------------------------------------ */
/* the captures of a line */

struct Capture {
	XferLine *line;
	FILE *f;                /* Capture Text's file, or NULL */
	bool f_paused;
	char path[MAX_PATH];
	bool prn, prn_paused;   /* Capture to Printer */
	TextFilter filter;
	WText prn_text;
	WText remote;           /* what the remote printed */
	DWORD remote_at;        /* when it last printed */
};

static Capture caps[2];

static Capture *of(XferLine *l)
{
	if (!l)
		return NULL;
	for (int i = 0; i < 2; i++)
		if (caps[i].line == l)
			return &caps[i];
	for (int i = 0; i < 2; i++)
		if (!caps[i].line) {
			caps[i].line = l;
			return &caps[i];
		}
	return NULL;
}

static void ini(char *out, int n)
{
	char dir[MAX_PATH];
	app_data_dir(dir, sizeof(dir));
	_snprintf(out, n, "%s" DIR_SEP "dmodem.ini", dir);
	out[n - 1] = '\0';
}

/* the folder of the captures: the last one used, else Captures in the
   app's folder */
static void folder(char *out, int n)
{
	char path[MAX_PATH];
	ini(path, sizeof(path));
	GetPrivateProfileString("capture", "folder", "", out, n, path);
	if (out[0])
		return;
	char dir[MAX_PATH];
	app_data_dir(dir, sizeof(dir));
	_snprintf(out, n, "%s" DIR_SEP "Captures", dir);
	out[n - 1] = '\0';
	CreateDirectory(out, NULL);
}

void capture_feed(XferLine *l, const char *data, int n)
{
	Capture *c = of(l);
	if (!c || n <= 0)
		return;
	if (c->f && !c->f_paused) {
		if (fwrite(data, 1, (size_t)n, c->f) != (size_t)n) {
			fclose(c->f);
			c->f = NULL;
			app_message("Capture Text stopped: the file cannot be written");
		}
	}
	if (c->prn && !c->prn_paused)
		filter_bytes(&c->filter, &c->prn_text, data, n);
}

void capture_printed(const char *data, int n, void *ctx)
{
	Capture *c = of((XferLine *)ctx);
	if (!c)
		return;
	/* during Capture to Printer: in its text, where it came */
	if (c->prn && !c->prn_paused) {
		wt_add_utf8(&c->prn_text, data, n);
		return;
	}
	wt_add_utf8(&c->remote, data, n);
	c->remote_at = GetTickCount();
}

void capture_tick()
{
	for (int i = 0; i < 2; i++) {
		Capture *c = &caps[i];
		/* (the file as it comes, for a viewer of it) */
		if (c->f)
			fflush(c->f);
		if (!c->remote.n ||
		    GetTickCount() - c->remote_at < REMOTE_QUIET_MS)
			continue;
		/* during Capture to Printer, with what it prints */
		if (c->prn) {
			for (int k = 0; k < c->remote.n; k++)
				wt_add(&c->prn_text, c->remote.p[k]);
		} else if (print_default("D-Modem: printed by the remote", &c->remote)) {
			app_message("The remote's printing went to the printer");
		} else {
			app_message("The remote printed, but there is no printer");
		}
		wt_free(&c->remote);
	}
}

/* Capture Text: a file to start it to (text added to one that exists) */
void capture_text_start(XferLine *l)
{
	Capture *c = of(l);
	if (!c || c->f)
		return;
	char dir[MAX_PATH], file[MAX_PATH];
	folder(dir, sizeof(dir));
	if (c->path[0])
		_snprintf(file, sizeof(file), "%s", c->path);
	else
		_snprintf(file, sizeof(file), "%s" DIR_SEP "capture.txt", dir);
	file[sizeof(file) - 1] = '\0';
	OPENFILENAME o;
	memset(&o, 0, sizeof(o));
	o.lStructSize = sizeof(o);
	o.hwndOwner = app.main;
	o.lpstrFilter = "Text (*.txt)\0*.txt\0ANSI (*.ans)\0*.ans\0"
		"All files (*.*)\0*.*\0";
	o.lpstrFile = file;
	o.nMaxFile = sizeof(file);
	o.lpstrInitialDir = dir;
	o.lpstrTitle = "Capture Text (added to the end of a file that exists)";
	o.lpstrDefExt = "txt";
	o.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;
	if (!GetSaveFileName(&o))
		return;
	c->f = fopen(file, "ab");
	if (!c->f) {
		MessageBox(app.main, "D-Modem cannot open the file for the capture.",
			   "Capture Text", MB_OK | MB_ICONEXCLAMATION);
		return;
	}
	c->f_paused = false;
	_snprintf(c->path, sizeof(c->path), "%s", file);
	c->path[sizeof(c->path) - 1] = '\0';
	/* the folder kept for the next time */
	char *slash = strrchr(file, DIR_SEP[0]);
	if (slash) {
		*slash = '\0';
		char path[MAX_PATH];
		ini(path, sizeof(path));
		WritePrivateProfileString("capture", "folder", file, path);
	}
	char m[MAX_PATH + 64];
	_snprintf(m, sizeof(m), "Capturing %s to %s", l->what, c->path);
	m[sizeof(m) - 1] = '\0';
	app_message(m);
}

void capture_text_stop(XferLine *l)
{
	Capture *c = of(l);
	if (!c || !c->f)
		return;
	fclose(c->f);
	c->f = NULL;
	char m[MAX_PATH + 64];
	_snprintf(m, sizeof(m), "Capture Text stopped: %s", c->path);
	m[sizeof(m) - 1] = '\0';
	app_message(m);
}

void capture_text_pause(XferLine *l, bool pause)
{
	Capture *c = of(l);
	if (c && c->f) {
		c->f_paused = pause;
		fflush(c->f);
		app_message(pause ? "Capture Text paused" : "Capture Text resumed");
	}
}

int capture_text_state(XferLine *l)
{
	Capture *c = of(l);
	return !c || !c->f ? CAPTURE_OFF : c->f_paused ? CAPTURE_PAUSED : CAPTURE_ON;
}

/* Capture to Printer: on, or off and the text printed */
void capture_printer(XferLine *l, bool on)
{
	Capture *c = of(l);
	if (!c || c->prn == on)
		return;
	if (on) {
		c->prn = true;
		c->prn_paused = false;
		memset(&c->filter, 0, sizeof(c->filter));
		app_message("Capture to Printer started: the text prints when you stop");
		return;
	}
	c->prn = false;
	if (c->prn_text.n) {
		if (print_default("D-Modem: Capture to Printer", &c->prn_text))
			app_message("Capture to Printer stopped: the text went to the printer");
		else
			MessageBox(app.main, "D-Modem cannot print the capture. "
				   "Make sure that a printer is installed.",
				   "Capture to Printer", MB_OK | MB_ICONEXCLAMATION);
	} else {
		app_message("Capture to Printer stopped: there was no text");
	}
	wt_free(&c->prn_text);
}

void capture_printer_pause(XferLine *l, bool pause)
{
	Capture *c = of(l);
	if (c && c->prn) {
		c->prn_paused = pause;
		app_message(pause ? "Capture to Printer paused" :
			    "Capture to Printer resumed");
	}
}

int capture_printer_state(XferLine *l)
{
	Capture *c = of(l);
	return !c || !c->prn ? CAPTURE_OFF :
		c->prn_paused ? CAPTURE_PAUSED : CAPTURE_ON;
}

/* File > Print: the screen, or the selection */
void capture_print(XferLine *l)
{
	HWND v = l && l->view ? *l->view : NULL;
	if (!v)
		return;
	bool sel = false;
	HDC dc = printer_dc(true, termview_has_selection(v), &sel);
	if (!dc) {
		/* (the user cancelled, or there is no printer) */
		if (CommDlgExtendedError())
			MessageBox(app.main, "D-Modem cannot find a printer.",
				   "Print", MB_OK | MB_ICONEXCLAMATION);
		return;
	}
	char *s = termview_text(v, sel);
	WText t = { NULL, 0, 0 };
	if (s) {
		wt_add_utf8(&t, s, (int)strlen(s));
		free(s);
	}
	if (!print_text(dc, sel ? "D-Modem: selection" : "D-Modem: screen",
			t.p ? t.p : L"", t.n))
		MessageBox(app.main, "D-Modem cannot print.", "Print",
			   MB_OK | MB_ICONEXCLAMATION);
	DeleteDC(dc);
	wt_free(&t);
}

void capture_close_all()
{
	for (int i = 0; i < 2; i++) {
		if (caps[i].f)
			fclose(caps[i].f);
		caps[i].f = NULL;
		wt_free(&caps[i].prn_text);
		wt_free(&caps[i].remote);
	}
}
