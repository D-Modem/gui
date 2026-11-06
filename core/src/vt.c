/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026, D-Modem
 *
 * vt.c - a small ANSI terminal screen (dmodemctl.h)
 *
 * Enough of a VT100 and ANSI X3.64 for the modem's command mode and its
 * full-screen configurator (AT!CF): UTF-8 text, CR, LF, BS, TAB, the
 * cursor moves (CUP, CUU, CUD, CUF, CUB, CHA, VPA, ESC 7 and 8), erasing
 * (ED, EL, ECH), inserting and deleting (ICH, DCH, IL, DL), the scroll
 * region (DECSTBM), SGR's colours and bold, underline and reverse, the
 * cursor shown or hidden (?25), and the cursor position report the
 * modem asks for (DSR 6) to learn the screen's size.  What it does not
 * know it skips.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dmodemctl.h"
#include "util.h"

struct cell {
	uint32_t ch;
	int8_t fg, bg;
	uint8_t attr;
};

struct line {
	struct cell *c;
	int n;
};

enum { GROUND, ESCAPE, CSI, OSC, OSC_ESC, CHARSET };

#define MAX_PARAMS 16

struct dmc_vt {
	int cols, rows;
	struct cell *screen;            /* rows * cols */
	/* the scrollback, newest last */
	struct line *hist;
	int hist_n, hist_max;
	int cx, cy;                     /* the cursor */
	int wrap;                       /* the next character wraps */
	int top, bottom;                /* the scroll region, 0-based */
	int sx, sy;                     /* ESC 7's */
	int8_t fg, bg;
	uint8_t attr;
	int cursor_on;
	int changed;
	/* the parser */
	int state;
	int params[MAX_PARAMS], n_params;
	int private_mark;
	/* UTF-8 being read */
	uint32_t u;
	int u_left;
	dmc_buf reply, text;
};

static struct cell blank(const dmc_vt *t)
{
	struct cell c = { ' ', -1, t ? t->bg : -1, 0 };
	return c;
}

static struct cell *cell(dmc_vt *t, int y, int x)
{
	return &t->screen[y * t->cols + x];
}

static void clear_cells(dmc_vt *t, int y, int x0, int x1)
{
	struct cell b = blank(t);
	for (int x = x0; x < x1 && x < t->cols; x++)
		*cell(t, y, x) = b;
}

dmc_vt *dmc_vt_new(int cols, int rows, int scrollback)
{
	dmc_vt *t = calloc(1, sizeof(*t));
	if (!t)
		abort();
	t->hist_max = scrollback > 0 ? scrollback : 0;
	t->cols = cols > 0 ? cols : 80;
	t->rows = rows > 0 ? rows : 24;
	t->screen = calloc((size_t)(t->cols * t->rows), sizeof(struct cell));
	if (!t->screen)
		abort();
	dmc_vt_reset(t);
	return t;
}

static void free_hist(dmc_vt *t)
{
	for (int i = 0; i < t->hist_n; i++)
		free(t->hist[i].c);
	free(t->hist);
	t->hist = NULL;
	t->hist_n = 0;
}

void dmc_vt_free(dmc_vt *t)
{
	if (!t)
		return;
	free_hist(t);
	free(t->screen);
	buf_free(&t->reply);
	buf_free(&t->text);
	free(t);
}

void dmc_vt_reset(dmc_vt *t)
{
	t->fg = t->bg = -1;
	t->attr = 0;
	for (int y = 0; y < t->rows; y++)
		clear_cells(t, y, 0, t->cols);
	t->cx = t->cy = t->sx = t->sy = 0;
	t->wrap = 0;
	t->top = 0;
	t->bottom = t->rows - 1;
	t->cursor_on = 1;
	t->state = GROUND;
	t->u_left = 0;
	t->changed = 1;
}

void dmc_vt_resize(dmc_vt *t, int cols, int rows)
{
	if (cols < 2 || rows < 2 || (cols == t->cols && rows == t->rows))
		return;
	struct cell *s = calloc((size_t)(cols * rows), sizeof(struct cell));
	if (!s)
		abort();
	struct cell b = { ' ', -1, -1, 0 };
	for (int i = 0; i < cols * rows; i++)
		s[i] = b;
	/* the bottom of the old screen at the bottom of the new, where
	   the cursor's line would otherwise go */
	int shift = t->cy >= rows ? t->cy - rows + 1 : 0;
	for (int y = 0; y < rows && y + shift < t->rows; y++)
		for (int x = 0; x < cols && x < t->cols; x++)
			s[y * cols + x] = *cell(t, y + shift, x);
	free(t->screen);
	t->screen = s;
	t->cols = cols;
	t->rows = rows;
	t->cy -= shift;
	if (t->cx >= cols)
		t->cx = cols - 1;
	if (t->cy >= rows)
		t->cy = rows - 1;
	t->top = 0;
	t->bottom = rows - 1;
	t->wrap = 0;
	t->changed = 1;
}

int dmc_vt_cols(const dmc_vt *t) { return t->cols; }
int dmc_vt_rows(const dmc_vt *t) { return t->rows; }

/* the top line of the screen to the scrollback */
static void keep_line(dmc_vt *t, int y)
{
	if (!t->hist_max)
		return;
	if (t->hist_n == t->hist_max) {
		free(t->hist[0].c);
		memmove(t->hist, t->hist + 1,
			sizeof(*t->hist) * (size_t)(t->hist_n - 1));
		t->hist_n--;
	} else {
		struct line *h = realloc(t->hist, sizeof(*h) *
					 (size_t)(t->hist_n + 1));
		if (!h)
			abort();
		t->hist = h;
	}
	struct line *l = &t->hist[t->hist_n++];
	l->n = t->cols;
	l->c = malloc(sizeof(struct cell) * (size_t)t->cols);
	if (!l->c)
		abort();
	memcpy(l->c, cell(t, y, 0), sizeof(struct cell) * (size_t)t->cols);
}

/* the region from top to bottom up n lines */
static void scroll_up(dmc_vt *t, int top, int bottom, int n)
{
	for (int k = 0; k < n; k++) {
		if (top == 0 && bottom == t->rows - 1)
			keep_line(t, 0);
		memmove(cell(t, top, 0), cell(t, top + 1, 0),
			sizeof(struct cell) * (size_t)(t->cols * (bottom - top)));
		clear_cells(t, bottom, 0, t->cols);
	}
}

static void scroll_down(dmc_vt *t, int top, int bottom, int n)
{
	for (int k = 0; k < n; k++) {
		memmove(cell(t, top + 1, 0), cell(t, top, 0),
			sizeof(struct cell) * (size_t)(t->cols * (bottom - top)));
		clear_cells(t, top, 0, t->cols);
	}
}

static void line_feed(dmc_vt *t)
{
	if (t->cy == t->bottom)
		scroll_up(t, t->top, t->bottom, 1);
	else if (t->cy < t->rows - 1)
		t->cy++;
}

static void put(dmc_vt *t, uint32_t ch)
{
	if (t->wrap) {
		t->cx = 0;
		line_feed(t);
		t->wrap = 0;
	}
	struct cell *c = cell(t, t->cy, t->cx);
	c->ch = ch;
	c->fg = t->fg;
	c->bg = t->bg;
	c->attr = t->attr;
	if (t->cx == t->cols - 1)
		t->wrap = 1;
	else
		t->cx++;
}

static int param(const dmc_vt *t, int i, int def)
{
	return i < t->n_params && t->params[i] > 0 ? t->params[i] : def;
}

static void clamp_cursor(dmc_vt *t)
{
	if (t->cx < 0)
		t->cx = 0;
	if (t->cx >= t->cols)
		t->cx = t->cols - 1;
	if (t->cy < 0)
		t->cy = 0;
	if (t->cy >= t->rows)
		t->cy = t->rows - 1;
	t->wrap = 0;
}

static void sgr(dmc_vt *t)
{
	if (!t->n_params) {
		t->fg = t->bg = -1;
		t->attr = 0;
		return;
	}
	for (int i = 0; i < t->n_params; i++) {
		int p = t->params[i] < 0 ? 0 : t->params[i];
		if (p == 0) {
			t->fg = t->bg = -1;
			t->attr = 0;
		} else if (p == 1) {
			t->attr |= DMC_BOLD;
		} else if (p == 4) {
			t->attr |= DMC_UNDERLINE;
		} else if (p == 7) {
			t->attr |= DMC_REVERSE;
		} else if (p == 22) {
			t->attr &= (uint8_t)~DMC_BOLD;
		} else if (p == 24) {
			t->attr &= (uint8_t)~DMC_UNDERLINE;
		} else if (p == 27) {
			t->attr &= (uint8_t)~DMC_REVERSE;
		} else if (p >= 30 && p <= 37) {
			t->fg = (int8_t)(p - 30);
		} else if (p == 39) {
			t->fg = -1;
		} else if (p >= 40 && p <= 47) {
			t->bg = (int8_t)(p - 40);
		} else if (p == 49) {
			t->bg = -1;
		} else if (p >= 90 && p <= 97) {
			t->fg = (int8_t)(p - 90 + 8);
		} else if (p >= 100 && p <= 107) {
			t->bg = (int8_t)(p - 100 + 8);
		} else if ((p == 38 || p == 48) && i + 1 < t->n_params) {
			/* 256 colours: the first 16 kept, the rest skipped */
			if (t->params[i + 1] == 5 && i + 2 < t->n_params) {
				int c = t->params[i + 2];
				if (c >= 0 && c < 16) {
					if (p == 38)
						t->fg = (int8_t)c;
					else
						t->bg = (int8_t)c;
				}
				i += 2;
			} else if (t->params[i + 1] == 2) {
				i += 4;
			}
		}
	}
}

static void csi(dmc_vt *t, char final)
{
	int n = param(t, 0, 1);
	switch (final) {
	case 'A':
		t->cy -= n;
		if (t->cy < t->top && t->cy + n >= t->top)
			t->cy = t->top;
		clamp_cursor(t);
		break;
	case 'B':
		t->cy += n;
		if (t->cy > t->bottom && t->cy - n <= t->bottom)
			t->cy = t->bottom;
		clamp_cursor(t);
		break;
	case 'C':
		t->cx += n;
		clamp_cursor(t);
		break;
	case 'D':
		t->cx -= n;
		clamp_cursor(t);
		break;
	case 'E':
		t->cy += n;
		t->cx = 0;
		clamp_cursor(t);
		break;
	case 'F':
		t->cy -= n;
		t->cx = 0;
		clamp_cursor(t);
		break;
	case 'G':
	case '`':
		t->cx = n - 1;
		clamp_cursor(t);
		break;
	case 'd':
		t->cy = n - 1;
		clamp_cursor(t);
		break;
	case 'H':
	case 'f':
		t->cy = param(t, 0, 1) - 1;
		t->cx = param(t, 1, 1) - 1;
		clamp_cursor(t);
		break;
	case 'J': {
		int p = t->n_params ? t->params[0] : 0;
		if (p == 0) {
			clear_cells(t, t->cy, t->cx, t->cols);
			for (int y = t->cy + 1; y < t->rows; y++)
				clear_cells(t, y, 0, t->cols);
		} else if (p == 1) {
			for (int y = 0; y < t->cy; y++)
				clear_cells(t, y, 0, t->cols);
			clear_cells(t, t->cy, 0, t->cx + 1);
		} else if (p == 2 || p == 3) {
			for (int y = 0; y < t->rows; y++)
				clear_cells(t, y, 0, t->cols);
		}
		break;
	}
	case 'K': {
		int p = t->n_params ? t->params[0] : 0;
		if (p == 0)
			clear_cells(t, t->cy, t->cx, t->cols);
		else if (p == 1)
			clear_cells(t, t->cy, 0, t->cx + 1);
		else if (p == 2)
			clear_cells(t, t->cy, 0, t->cols);
		break;
	}
	case 'X':
		clear_cells(t, t->cy, t->cx, t->cx + n);
		break;
	case '@': {
		if (n > t->cols - t->cx)
			n = t->cols - t->cx;
		memmove(cell(t, t->cy, t->cx + n), cell(t, t->cy, t->cx),
			sizeof(struct cell) * (size_t)(t->cols - t->cx - n));
		clear_cells(t, t->cy, t->cx, t->cx + n);
		break;
	}
	case 'P': {
		if (n > t->cols - t->cx)
			n = t->cols - t->cx;
		memmove(cell(t, t->cy, t->cx), cell(t, t->cy, t->cx + n),
			sizeof(struct cell) * (size_t)(t->cols - t->cx - n));
		clear_cells(t, t->cy, t->cols - n, t->cols);
		break;
	}
	case 'L':
		if (t->cy >= t->top && t->cy <= t->bottom) {
			if (n > t->bottom - t->cy + 1)
				n = t->bottom - t->cy + 1;
			scroll_down(t, t->cy, t->bottom, n);
		}
		break;
	case 'M':
		if (t->cy >= t->top && t->cy <= t->bottom) {
			if (n > t->bottom - t->cy + 1)
				n = t->bottom - t->cy + 1;
			for (int k = 0; k < n; k++) {
				memmove(cell(t, t->cy, 0), cell(t, t->cy + 1, 0),
					sizeof(struct cell) *
					(size_t)(t->cols * (t->bottom - t->cy)));
				clear_cells(t, t->bottom, 0, t->cols);
			}
		}
		break;
	case 'S':
		scroll_up(t, t->top, t->bottom, n);
		break;
	case 'T':
		scroll_down(t, t->top, t->bottom, n);
		break;
	case 'm':
		sgr(t);
		break;
	case 'r': {
		int top = param(t, 0, 1) - 1, bottom = param(t, 1, t->rows) - 1;
		if (bottom >= t->rows)
			bottom = t->rows - 1;
		if (top < bottom) {
			t->top = top;
			t->bottom = bottom;
			t->cx = t->cy = 0;
			t->wrap = 0;
		}
		break;
	}
	case 's':
		t->sx = t->cx;
		t->sy = t->cy;
		break;
	case 'u':
		t->cx = t->sx;
		t->cy = t->sy;
		clamp_cursor(t);
		break;
	case 'n':
		if (t->n_params && t->params[0] == 6) {
			char r[32];
			snprintf(r, sizeof(r), "\x1b[%d;%dR", t->cy + 1, t->cx + 1);
			buf_adds(&t->reply, r);
		} else if (t->n_params && t->params[0] == 5) {
			buf_adds(&t->reply, "\x1b[0n");
		}
		break;
	case 'c':
		if (!t->private_mark)
			buf_adds(&t->reply, "\x1b[?1;2c");
		break;
	case 'h':
	case 'l':
		if (t->private_mark == '?') {
			for (int i = 0; i < t->n_params; i++) {
				if (t->params[i] == 25)
					t->cursor_on = final == 'h';
				else if (t->params[i] == 1049 ||
					 t->params[i] == 47 ||
					 t->params[i] == 1047) {
					for (int y = 0; y < t->rows; y++)
						clear_cells(t, y, 0, t->cols);
				}
			}
		}
		break;
	default:
		break;
	}
}

static void control(dmc_vt *t, uint32_t c)
{
	switch (c) {
	case '\r':
		t->cx = 0;
		t->wrap = 0;
		break;
	case '\n':
	case '\v':
	case '\f':
		line_feed(t);
		t->wrap = 0;
		break;
	case '\b':
		if (t->cx > 0)
			t->cx--;
		t->wrap = 0;
		break;
	case '\t':
		t->cx = (t->cx / 8 + 1) * 8;
		if (t->cx >= t->cols)
			t->cx = t->cols - 1;
		t->wrap = 0;
		break;
	case 0x1b:
		t->state = ESCAPE;
		break;
	default:
		break;
	}
}

static void byte(dmc_vt *t, unsigned char b)
{
	switch (t->state) {
	case ESCAPE:
		t->state = GROUND;
		if (b == '[') {
			t->state = CSI;
			t->n_params = 0;
			t->private_mark = 0;
			memset(t->params, -1, sizeof(t->params));
		} else if (b == ']') {
			t->state = OSC;
		} else if (b == '(' || b == ')' || b == '*' || b == '+') {
			t->state = CHARSET;
		} else if (b == '7') {
			t->sx = t->cx;
			t->sy = t->cy;
		} else if (b == '8') {
			t->cx = t->sx;
			t->cy = t->sy;
			clamp_cursor(t);
		} else if (b == 'D') {
			line_feed(t);
		} else if (b == 'E') {
			t->cx = 0;
			line_feed(t);
		} else if (b == 'M') {
			if (t->cy == t->top)
				scroll_down(t, t->top, t->bottom, 1);
			else if (t->cy > 0)
				t->cy--;
		} else if (b == 'c') {
			dmc_vt_reset(t);
		}
		return;
	case CHARSET:
		t->state = GROUND;
		return;
	case OSC:
		if (b == 7)
			t->state = GROUND;
		else if (b == 0x1b)
			t->state = OSC_ESC;
		return;
	case OSC_ESC:
		t->state = b == '\\' ? GROUND : OSC;
		return;
	case CSI:
		if (b >= '0' && b <= '9') {
			if (!t->n_params)
				t->n_params = 1;
			int *p = &t->params[t->n_params - 1];
			if (*p < 0)
				*p = 0;
			if (*p < 100000)
				*p = *p * 10 + (b - '0');
		} else if (b == ';' || b == ':') {
			if (!t->n_params)
				t->n_params = 1;
			if (t->n_params < MAX_PARAMS)
				t->n_params++;
		} else if (b == '?' || b == '>' || b == '=' || b == '<') {
			t->private_mark = b;
		} else if (b >= 0x40 && b <= 0x7e) {
			t->state = GROUND;
			csi(t, (char)b);
		} else if (b == 0x1b) {
			t->state = ESCAPE;
		} else if (b < 0x20) {
			control(t, b);
		}
		return;
	default:
		break;
	}
	/* the ground state: UTF-8 */
	if (t->u_left) {
		if ((b & 0xC0) == 0x80) {
			t->u = (t->u << 6) | (b & 0x3F);
			if (--t->u_left == 0)
				put(t, t->u);
			return;
		}
		t->u_left = 0;
		put(t, 0xFFFD);
	}
	if (b < 0x20 || b == 0x7f) {
		control(t, b);
	} else if (b < 0x80) {
		put(t, b);
	} else if ((b & 0xE0) == 0xC0) {
		t->u = b & 0x1F;
		t->u_left = 1;
	} else if ((b & 0xF0) == 0xE0) {
		t->u = b & 0x0F;
		t->u_left = 2;
	} else if ((b & 0xF8) == 0xF0) {
		t->u = b & 0x07;
		t->u_left = 3;
	} else {
		put(t, 0xFFFD);
	}
}

void dmc_vt_feed(dmc_vt *t, const char *data, int n)
{
	for (int i = 0; i < n; i++)
		byte(t, (unsigned char)data[i]);
	if (n > 0)
		t->changed = 1;
}

int dmc_vt_reply(dmc_vt *t, char *buf, int max)
{
	return buf_take(&t->reply, buf, max);
}

int dmc_vt_history(const dmc_vt *t)
{
	return t->hist_n;
}

static const struct cell *get(const dmc_vt *t, int row, int col)
{
	static const struct cell none = { ' ', -1, -1, 0 };
	if (row >= 0) {
		if (row >= t->rows || col < 0 || col >= t->cols)
			return &none;
		return &t->screen[row * t->cols + col];
	}
	int h = t->hist_n + row;
	if (h < 0 || col < 0 || col >= t->hist[h].n)
		return &none;
	return &t->hist[h].c[col];
}

uint32_t dmc_vt_char(const dmc_vt *t, int row, int col)
{
	return get(t, row, col)->ch;
}

int dmc_vt_fg(const dmc_vt *t, int row, int col)
{
	return get(t, row, col)->fg;
}

int dmc_vt_bg(const dmc_vt *t, int row, int col)
{
	return get(t, row, col)->bg;
}

int dmc_vt_attr(const dmc_vt *t, int row, int col)
{
	return get(t, row, col)->attr;
}

static void utf8(dmc_buf *b, uint32_t c)
{
	char o[4];
	if (c < 0x80) {
		buf_addc(b, (char)c);
	} else if (c < 0x800) {
		o[0] = (char)(0xC0 | (c >> 6));
		o[1] = (char)(0x80 | (c & 0x3F));
		buf_add(b, o, 2);
	} else if (c < 0x10000) {
		o[0] = (char)(0xE0 | (c >> 12));
		o[1] = (char)(0x80 | ((c >> 6) & 0x3F));
		o[2] = (char)(0x80 | (c & 0x3F));
		buf_add(b, o, 3);
	} else {
		o[0] = (char)(0xF0 | (c >> 18));
		o[1] = (char)(0x80 | ((c >> 12) & 0x3F));
		o[2] = (char)(0x80 | ((c >> 6) & 0x3F));
		o[3] = (char)(0x80 | (c & 0x3F));
		buf_add(b, o, 4);
	}
}

const char *dmc_vt_text(dmc_vt *t, int row)
{
	buf_clear(&t->text);
	int n = row >= 0 ? t->cols : (t->hist_n + row >= 0 ?
				       t->hist[t->hist_n + row].n : 0);
	while (n > 0 && get(t, row, n - 1)->ch == ' ')
		n--;
	for (int x = 0; x < n; x++)
		utf8(&t->text, get(t, row, x)->ch);
	return buf_str(&t->text);
}

int dmc_vt_cursor_row(const dmc_vt *t) { return t->cy; }
int dmc_vt_cursor_col(const dmc_vt *t) { return t->cx; }
int dmc_vt_cursor_visible(const dmc_vt *t) { return t->cursor_on; }

int dmc_vt_changed(dmc_vt *t)
{
	int c = t->changed;
	t->changed = 0;
	return c;
}

int dmc_vt_key(const dmc_vt *t, int key, char *buf, int max)
{
	static const char *const seq[] = {
		[DMC_KEY_UP] = "\x1b[A", [DMC_KEY_DOWN] = "\x1b[B",
		[DMC_KEY_RIGHT] = "\x1b[C", [DMC_KEY_LEFT] = "\x1b[D",
		[DMC_KEY_HOME] = "\x1b[H", [DMC_KEY_END] = "\x1b[F",
		[DMC_KEY_PGUP] = "\x1b[5~", [DMC_KEY_PGDN] = "\x1b[6~",
		[DMC_KEY_INSERT] = "\x1b[2~", [DMC_KEY_DELETE] = "\x1b[3~",
		[DMC_KEY_BACKTAB] = "\x1b[Z",
		[DMC_KEY_F1] = "\x1bOP", [DMC_KEY_F2] = "\x1bOQ",
		[DMC_KEY_F3] = "\x1bOR", [DMC_KEY_F4] = "\x1bOS",
		[DMC_KEY_F5] = "\x1b[15~", [DMC_KEY_F6] = "\x1b[17~",
		[DMC_KEY_F7] = "\x1b[18~", [DMC_KEY_F8] = "\x1b[19~",
		[DMC_KEY_F9] = "\x1b[20~", [DMC_KEY_F10] = "\x1b[21~",
		[DMC_KEY_ENTER] = "\r", [DMC_KEY_BACKSPACE] = "\x7f",
		[DMC_KEY_TAB] = "\t", [DMC_KEY_ESCAPE] = "\x1b",
	};
	(void)t;
	if (key <= 0 || key > DMC_KEY_ESCAPE || !seq[key])
		return 0;
	int n = (int)strlen(seq[key]);
	if (n > max)
		return 0;
	memcpy(buf, seq[key], (size_t)n);
	return n;
}

uint32_t dmc_vt_palette(int c)
{
	/* the CGA's colours, as the configurator was drawn for them */
	static const uint32_t pal[16] = {
		0x000000, 0xAA0000, 0x00AA00, 0xAA5500,
		0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA,
		0x555555, 0xFF5555, 0x55FF55, 0xFFFF55,
		0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF,
	};
	return c >= 0 && c < 16 ? pal[c] : 0;
}
