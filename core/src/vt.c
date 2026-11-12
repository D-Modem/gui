/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * vt.c - the terminal's screen (dmodemctl.h's dmc_vt): the main and
 * alternate screens, the scrollback, the cursor and the modes the keys
 * and the view read, with the operations every kind of terminal is made
 * of (print, scroll, erase, insert, move the cursor).  What the
 * remote's characters mean is the interpreter's, chosen by the profile
 * (vt_profiles.c): DEC's VT line and xterm, the PC's ANSI-BBS, the home
 * computers' PETSCII and ATASCII, videotex, the graphics terminals.
 *
 * A port of the D-Modem app's TerminalEmulator, Screen and
 * TerminalBridge (D-Modem/app: shared/.../terminal/).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vtint.h"

static void *xcalloc(size_t n, size_t s)
{
	void *p = calloc(n ? n : 1, s ? s : 1);
	if (!p)
		abort();
	return p;
}

static void *xrealloc(void *p, size_t s)
{
	p = realloc(p, s ? s : 1);
	if (!p)
		abort();
	return p;
}

/* ------------------------------------------------------------------ */
/* pictures and links */

struct vt_image *image_new(int w, int h, int cell_w, int cell_h, int picture)
{
	static uint32_t next_id;
	struct vt_image *img = xcalloc(1, sizeof(*img));
	img->refs = 1;
	img->id = ++next_id;
	img->width = w;
	img->height = h;
	img->pixels = xcalloc((size_t)w * (size_t)h, sizeof(uint32_t));
	img->cell_w = cell_w > 0 ? cell_w : 1;
	img->cell_h = cell_h > 0 ? cell_h : 1;
	img->picture = picture;
	return img;
}

void image_unref(struct vt_image *img)
{
	if (img && --img->refs <= 0) {
		free(img->pixels);
		free(img);
	}
}

struct vt_link *link_new(const char *uri)
{
	struct vt_link *l = xcalloc(1, sizeof(*l));
	l->refs = 1;
	l->uri = dup_str(uri);
	return l;
}

void link_unref(struct vt_link *l)
{
	if (l && --l->refs <= 0) {
		free(l->uri);
		free(l);
	}
}

/* ------------------------------------------------------------------ */
/* lines */

static void extra_free(struct vt_extra *e)
{
	if (!e)
		return;
	free(e->comb);
	image_unref(e->img);
	link_unref(e->link);
	free(e);
}

struct vt_line *line_new(int cols, vt_style st)
{
	struct vt_line *l = xcalloc(1, sizeof(*l));
	int i;
	l->cols = cols;
	l->ch = xcalloc((size_t)cols, sizeof(int32_t));
	l->st = xcalloc((size_t)cols, sizeof(vt_style));
	for (i = 0; i < cols; i++)
		l->st[i] = st;
	return l;
}

static void drop_extras(struct vt_line *l, int from, int to)
{
	int i;
	if (!l->ex)
		return;
	for (i = from; i < to; i++) {
		extra_free(l->ex[i]);
		l->ex[i] = NULL;
	}
}

void line_free(struct vt_line *l)
{
	if (!l)
		return;
	drop_extras(l, 0, l->cols);
	free(l->ex);
	free(l->ch);
	free(l->st);
	free(l);
}

struct vt_extra *line_extra(const struct vt_line *l, int i)
{
	if (!l->ex || i < 0 || i >= l->cols)
		return NULL;
	return l->ex[i];
}

struct vt_extra *line_extra_for(struct vt_line *l, int i)
{
	if (!l->ex)
		l->ex = xcalloc((size_t)l->cols, sizeof(struct vt_extra *));
	if (!l->ex[i])
		l->ex[i] = xcalloc(1, sizeof(struct vt_extra));
	return l->ex[i];
}

void extra_assign(struct vt_line *l, int i, const struct vt_extra *e)
{
	struct vt_extra *d;
	if (!e) {
		drop_extras(l, i, i + 1);
		return;
	}
	d = line_extra_for(l, i);
	free(d->comb);
	d->comb = NULL;
	d->ncomb = 0;
	if (e->ncomb) {
		d->comb = xcalloc((size_t)e->ncomb, sizeof(uint32_t));
		memcpy(d->comb, e->comb, (size_t)e->ncomb * sizeof(uint32_t));
		d->ncomb = e->ncomb;
	}
	if (e->img)
		e->img->refs++;
	image_unref(d->img);
	d->img = e->img;
	d->tx = e->tx;
	d->ty = e->ty;
	if (e->link)
		e->link->refs++;
	link_unref(d->link);
	d->link = e->link;
}

void line_set(struct vt_line *l, int i, int32_t cp, vt_style st)
{
	if (i < 0 || i >= l->cols)
		return;
	l->ch[i] = cp;
	l->st[i] = st;
	drop_extras(l, i, i + 1);
}

void line_clear(struct vt_line *l, int from, int to, vt_style st)
{
	int i;
	if (from < 0)
		from = 0;
	if (to > l->cols)
		to = l->cols;
	for (i = from; i < to; i++) {
		l->ch[i] = 0;
		l->st[i] = st;
	}
	if (from < to)
		drop_extras(l, from, to);
}

void line_blank(struct vt_line *l, vt_style st)
{
	line_clear(l, 0, l->cols, st);
	l->wrapped = 0;
	l->size = LS_SINGLE;
}

void line_insert_cells(struct vt_line *l, int at, int n, vt_style st, int end)
{
	int e = end < l->cols ? end : l->cols, k;
	if (at < 0 || at >= e || n <= 0)
		return;
	k = n < e - at ? n : e - at;
	/* (the ones pushed off the end freed) */
	drop_extras(l, e - k, e);
	memmove(l->ch + at + k, l->ch + at, (size_t)(e - k - at) * sizeof(int32_t));
	memmove(l->st + at + k, l->st + at, (size_t)(e - k - at) * sizeof(vt_style));
	if (l->ex) {
		memmove(l->ex + at + k, l->ex + at,
			(size_t)(e - k - at) * sizeof(struct vt_extra *));
		memset(l->ex + at, 0, (size_t)k * sizeof(struct vt_extra *));
	}
	line_clear(l, at, at + k, st);
}

void line_delete_cells(struct vt_line *l, int at, int n, vt_style st, int end)
{
	int e = end < l->cols ? end : l->cols, k;
	if (at < 0 || at >= e || n <= 0)
		return;
	k = n < e - at ? n : e - at;
	drop_extras(l, at, at + k);
	memmove(l->ch + at, l->ch + at + k, (size_t)(e - at - k) * sizeof(int32_t));
	memmove(l->st + at, l->st + at + k, (size_t)(e - at - k) * sizeof(vt_style));
	if (l->ex) {
		memmove(l->ex + at, l->ex + at + k,
			(size_t)(e - at - k) * sizeof(struct vt_extra *));
		memset(l->ex + e - k, 0, (size_t)k * sizeof(struct vt_extra *));
	}
	line_clear(l, e - k, e, st);
}

void line_resize(struct vt_line *l, int n)
{
	int keep = n < l->cols ? n : l->cols, i;
	if (n == l->cols)
		return;
	if (n < l->cols)
		drop_extras(l, n, l->cols);
	l->ch = xrealloc(l->ch, (size_t)n * sizeof(int32_t));
	l->st = xrealloc(l->st, (size_t)n * sizeof(vt_style));
	for (i = keep; i < n; i++) {
		l->ch[i] = 0;
		l->st[i] = ST_NORMAL;
	}
	if (l->ex) {
		l->ex = xrealloc(l->ex, (size_t)n * sizeof(struct vt_extra *));
		for (i = keep; i < n; i++)
			l->ex[i] = NULL;
	}
	l->cols = n;
	/* (a wide character cut in half at the new edge: gone) */
	if (n > 0 && l->ch[n - 1] > 0 && wcwidth_of((uint32_t)l->ch[n - 1]) == 2)
		l->ch[n - 1] = 0;
}

void line_copy_from(struct vt_line *l, const struct vt_line *o)
{
	int n = l->cols < o->cols ? l->cols : o->cols, i;
	memcpy(l->ch, o->ch, (size_t)n * sizeof(int32_t));
	memcpy(l->st, o->st, (size_t)n * sizeof(vt_style));
	drop_extras(l, 0, l->cols);
	if (o->ex)
		for (i = 0; i < n; i++)
			if (o->ex[i])
				extra_assign(l, i, o->ex[i]);
	l->wrapped = o->wrapped;
	l->size = o->size;
}

void utf8_put(dmc_buf *b, uint32_t c)
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

void line_text(const struct vt_line *l, int from, int to, dmc_buf *b)
{
	int end = to < l->cols ? to : l->cols, i, k;
	if (from < 0)
		from = 0;
	while (end > from && l->ch[end - 1] == 0)
		end--;
	for (i = from; i < end; i++) {
		int32_t cp = l->ch[i];
		const struct vt_extra *e;
		if (cp == WIDE_TAIL)
			continue;
		utf8_put(b, cp == 0 ? ' ' : (uint32_t)cp);
		e = line_extra(l, i);
		if (e)
			for (k = 0; k < e->ncomb; k++)
				utf8_put(b, e->comb[k]);
	}
}

/* ------------------------------------------------------------------ */
/* bitmap fonts */

int font_index(const struct vt_font *f, uint32_t cp)
{
	int lo = 0, hi = f->n - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		uint32_t c = f->codes[mid];
		if (c < cp)
			lo = mid + 1;
		else if (c > cp)
			hi = mid - 1;
		else
			return mid;
	}
	return -1;
}

uint32_t font_row(const struct vt_font *f, uint32_t cp, int row)
{
	int i = font_index(f, cp);
	if (i < 0 || row < 0 || row >= f->h)
		return 0;
	return f->rows8 ? f->rows8[i * f->h + row] : f->rows32[i * f->h + row];
}

/* ------------------------------------------------------------------ */
/* the screens */

static struct vt_line **screen_new(int cols, int rows)
{
	struct vt_line **s = xcalloc((size_t)rows, sizeof(*s));
	int i;
	for (i = 0; i < rows; i++)
		s[i] = line_new(cols, ST_NORMAL);
	return s;
}

static void screen_free(struct vt_line **s, int rows)
{
	int i;
	if (!s)
		return;
	for (i = 0; i < rows; i++)
		line_free(s[i]);
	free(s);
}

static struct vt_line *hist_at(const dmc_vt *t, int i)
{
	return t->hist[(t->hist_first + i) % t->hist_max];
}

/* a line onto the scrollback (taken over), the oldest dropped */
static void hist_push(dmc_vt *t, struct vt_line *l)
{
	if (t->hist_max <= 0) {
		line_free(l);
		return;
	}
	if (t->hist_n == t->hist_max) {
		line_free(t->hist[t->hist_first]);
		t->hist[t->hist_first] = l;
		t->hist_first = (t->hist_first + 1) % t->hist_max;
	} else {
		t->hist[(t->hist_first + t->hist_n) % t->hist_max] = l;
		t->hist_n++;
	}
}

/* the newest scrollback line taken back (NULL for none) */
static struct vt_line *hist_pop(dmc_vt *t)
{
	struct vt_line *l;
	if (t->hist_n == 0)
		return NULL;
	l = hist_at(t, t->hist_n - 1);
	t->hist_n--;
	return l;
}

static void hist_clear(dmc_vt *t)
{
	while (t->hist_n > 0)
		line_free(hist_pop(t));
	t->hist_first = 0;
}

static void reset_tabs(dmc_vt *t)
{
	int i;
	free(t->tabs);
	t->tabs = xcalloc((size_t)t->cols, 1);
	for (i = 0; i < t->cols; i++)
		t->tabs[i] = i % 8 == 0 && i > 0;
}

int vt_alt_screen(const dmc_vt *t)
{
	return t->screen == t->alt_s;
}

struct vt_line *vt_line(const dmc_vt *t, int row)
{
	if (row >= 0)
		return t->screen[row];
	return hist_at(t, t->hist_n + row);
}

static int history_size(const dmc_vt *t)
{
	return vt_alt_screen(t) ? 0 : t->hist_n;
}

/* ------------------------------------------------------------------ */
/* colours */

void vt_set_color_override(dmc_vt *t, int i, uint32_t argb)
{
	if (i >= 0 && i < 259)
		t->color_over[i] = argb | 0xff000000u;
}

void vt_reset_colors(dmc_vt *t, int from, int to)
{
	int i;
	for (i = from; i <= to; i++)
		if (i >= 0 && i < 259)
			t->color_over[i] = VT_NO_COLOR;
}

uint32_t vt_color_override(const dmc_vt *t, int i)
{
	return i >= 0 && i < 259 ? t->color_over[i] : VT_NO_COLOR;
}

/* the theme's colour for c (Style's encoding), opaque ARGB */
static uint32_t theme_color(const dmc_vt *t, int c, int is_fg)
{
	static const int steps[6] = { 0, 95, 135, 175, 215, 255 };
	if (c == ST_DEFAULT_FG)
		return 0xff000000u | t->theme_fg;
	if (c == ST_DEFAULT_BG)
		return 0xff000000u | t->theme_bg;
	if (c & ST_RGB)
		return 0xff000000u | ((uint32_t)c & 0xffffff);
	if (c >= 0 && c < 16)
		return 0xff000000u | t->theme_pal[c];
	if (c >= 16 && c < 232) {
		int n = c - 16;
		return 0xff000000u | ((uint32_t)steps[n / 36] << 16) |
			((uint32_t)steps[(n / 6) % 6] << 8) | (uint32_t)steps[n % 6];
	}
	if (c >= 232 && c < 256) {
		uint32_t v = (uint32_t)(8 + (c - 232) * 10);
		return 0xff000000u | (v << 16) | (v << 8) | v;
	}
	return 0xff000000u | (is_fg ? t->theme_fg : t->theme_bg);
}

uint32_t vt_shown_color(const dmc_vt *t, int i)
{
	uint32_t o = vt_color_override(t, i);
	if (o != VT_NO_COLOR)
		return o;
	if (i == 258)
		return 0xff000000u | t->theme_cursor;
	return theme_color(t, i == 256 ? ST_DEFAULT_FG : i == 257 ? ST_DEFAULT_BG : i,
			   i != 257);
}

/* colour c (Style's encoding) as the screen shows it: the remote's
   override if it set one, else the theme's */
static uint32_t resolve(const dmc_vt *t, int c, int is_fg)
{
	int i = -1;
	if (c == ST_DEFAULT_FG)
		i = 256;
	else if (c == ST_DEFAULT_BG)
		i = 257;
	else if (!(c & ST_RGB) && c >= 0 && c < 256)
		i = c;
	if (i >= 0) {
		uint32_t o = vt_color_override(t, i);
		if (o != VT_NO_COLOR)
			return o;
	}
	return theme_color(t, c, is_fg);
}

/* ------------------------------------------------------------------ */
/* the app's notices */

void vt_reply(dmc_vt *t, const char *s)
{
	const unsigned char *p = (const unsigned char *)s;
	for (; *p; p++) {
		if (*p == 0x1b && t->eight_bit_replies && p[1] >= 0x40 &&
		    p[1] <= 0x5f) {
			buf_addc(&t->reply, (char)(p[1] + 0x40));
			p++;
		} else {
			buf_addc(&t->reply, (char)*p);
		}
	}
}

void vt_replyf(dmc_vt *t, const char *fmt, ...)
{
	char b[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	vt_reply(t, b);
}

void vt_bell(dmc_vt *t)
{
	t->bells++;
}

void vt_add_music(dmc_vt *t, double hz, int ms, int rest_ms)
{
	if (t->music_n >= 4096)
		return;
	if (t->music_n + 3 > t->music_cap) {
		t->music_cap = t->music_cap ? t->music_cap * 2 : 96;
		t->music = xrealloc(t->music, (size_t)t->music_cap * sizeof(uint32_t));
	}
	t->music[t->music_n++] = (uint32_t)(hz * 100 + 0.5);
	t->music[t->music_n++] = (uint32_t)(ms > 0 ? ms : 0);
	t->music[t->music_n++] = (uint32_t)(rest_ms > 0 ? rest_ms : 0);
}

void vt_print_out(dmc_vt *t, const char *s)
{
	if (t->print.n < 1 << 20)
		buf_adds(&t->print, s);
}

void vt_set_title(dmc_vt *t, const char *s)
{
	buf_clear(&t->title);
	buf_adds(&t->title, s);
	t->title_changed = 1;
}

/* ------------------------------------------------------------------ */
/* images */

void vt_show_picture(dmc_vt *t, struct vt_image *img)
{
	int cw = img->cell_w, ch = img->cell_h, tx, ty, x, y;
	for (ty = 0; ty < t->rows; ty++) {
		struct vt_line *line = t->screen[ty];
		for (tx = 0; tx < t->cols; tx++) {
			int drawn = 0;
			struct vt_extra *e;
			for (y = ty * ch; y < (ty + 1) * ch && y < img->height && !drawn; y++) {
				const uint32_t *row = img->pixels + (size_t)y * img->width;
				for (x = tx * cw; x < (tx + 1) * cw && x < img->width; x++)
					if (row[x]) {
						drawn = 1;
						break;
					}
			}
			e = line_extra(line, tx);
			if (drawn) {
				line_set(line, tx, 0, st_erased(t->style));
				e = line_extra_for(line, tx);
				img->refs++;
				e->img = img;
				e->tx = tx;
				e->ty = ty;
			} else if (e && e->img && e->img->picture) {
				image_unref(e->img);
				e->img = NULL;
			}
		}
	}
	image_unref(img);
}

static void tile(dmc_vt *t, struct vt_line *line, int x, struct vt_image *img,
		 int tx, int ty)
{
	struct vt_extra *e;
	line_set(line, x, 0, st_erased(t->style));
	e = line_extra_for(line, x);
	img->refs++;
	e->img = img;
	e->tx = tx;
	e->ty = ty;
}

void vt_place_image(dmc_vt *t, struct vt_image *img)
{
	int tw = (img->width + img->cell_w - 1) / img->cell_w;
	int th = (img->height + img->cell_h - 1) / img->cell_h;
	int tx, ty, x0;
	if (!t->sixel_scrolling) {
		for (ty = 0; ty < th && ty < t->rows; ty++)
			for (tx = 0; tx < tw && tx < t->cols; tx++)
				tile(t, t->screen[ty], tx, img, tx, ty);
		image_unref(img);
		return;
	}
	if (t->pending_wrap) {
		vt_carriage_return(t);
		vt_index(t);
	}
	x0 = t->cx;
	for (ty = 0; ty < th; ty++) {
		struct vt_line *line = t->screen[t->cy];
		for (tx = 0; tx < tw; tx++) {
			if (x0 + tx >= t->cols)
				break;
			tile(t, line, x0 + tx, img, tx, ty);
		}
		if (ty < th - 1)
			vt_index(t);
	}
	if (t->sixel_cursor_right) {
		t->cx = x0 + tw < t->cols - 1 ? x0 + tw : t->cols - 1;
	} else {
		vt_index(t);
		t->cx = x0;
	}
	t->pending_wrap = 0;
	image_unref(img);
}

/* ------------------------------------------------------------------ */
/* the screen's operations */

static int full_width(const dmc_vt *t)
{
	return t->left == 0 && t->right == t->cols - 1;
}

int vt_within_margins(const dmc_vt *t)
{
	return t->cx >= t->left && t->cx <= t->right;
}

int vt_line_width(const dmc_vt *t, int y)
{
	if (t->screen[y]->size != LS_SINGLE)
		return t->cols / 2 > 1 ? t->cols / 2 : 1;
	return t->cols;
}

static void unpair(dmc_vt *t, struct vt_line *line, int x)
{
	if (x >= t->cols)
		return;
	if (line->ch[x] == WIDE_TAIL && x > 0)
		line->ch[x - 1] = 0;
	if (x + 1 < t->cols && line->ch[x + 1] == WIDE_TAIL &&
	    line->ch[x] != WIDE_TAIL)
		line->ch[x + 1] = 0;
}

static void combine(dmc_vt *t, uint32_t cp)
{
	struct vt_line *line = t->screen[t->cy];
	int x = t->pending_wrap ? t->cx : t->cx - 1;
	struct vt_extra *e;
	if (x < 0)
		return;
	if (line->ch[x] == WIDE_TAIL && x > 0)
		x--;
	if (line->ch[x] == 0)
		return;
	e = line_extra_for(line, x);
	/* (a few marks at most: enough for any real cluster) */
	if (e->ncomb >= 16)
		return;
	e->comb = xrealloc(e->comb, (size_t)(e->ncomb + 1) * sizeof(uint32_t));
	e->comb[e->ncomb++] = cp;
	if (cp == 0x200D)
		t->join_next = 1;
}

void vt_print(dmc_vt *t, int32_t cp)
{
	int w = cp < 0x300 ? 1 : wcwidth_of((uint32_t)cp), lc, k;
	struct vt_line *line;
	if (w == 0 || t->join_next) {
		t->join_next = 0;
		combine(t, (uint32_t)cp);
		return;
	}
	t->last_char = cp;
	if (t->pending_wrap) {
		if (t->autowrap) {
			t->screen[t->cy]->wrapped = 1;
			vt_carriage_return(t);
			vt_index(t);
		}
		t->pending_wrap = 0;
	}
	lc = vt_line_width(t, t->cy);
	if (vt_within_margins(t) && !full_width(t) && t->right + 1 < lc)
		lc = t->right + 1;
	if (t->cx >= lc)
		t->cx = lc - 1;
	if (w == 2 && t->cx == lc - 1) {
		if (!t->autowrap || lc < 2)
			return;
		line_clear(t->screen[t->cy], t->cx, t->cols, t->style);
		t->screen[t->cy]->wrapped = 1;
		vt_carriage_return(t);
		vt_index(t);
	}
	line = t->screen[t->cy];
	if (t->insert_mode)
		line_insert_cells(line, t->cx, w, t->style, lc);
	unpair(t, line, t->cx);
	if (w == 2)
		unpair(t, line, t->cx + 1);
	line_set(line, t->cx, cp, t->style);
	if (w == 2)
		line_set(line, t->cx + 1, WIDE_TAIL, t->style);
	if (t->link)
		for (k = 0; k < w; k++) {
			struct vt_extra *e = line_extra_for(line, t->cx + k);
			t->link->refs++;
			link_unref(e->link);
			e->link = t->link;
		}
	if (t->cx + w >= lc) {
		t->cx = lc - 1;
		t->pending_wrap = 1;
	} else {
		t->cx += w;
	}
}

void vt_set_line_size(dmc_vt *t, int size)
{
	t->screen[t->cy]->size = size;
	if (t->cx >= vt_line_width(t, t->cy))
		t->cx = vt_line_width(t, t->cy) - 1;
}

void vt_set_column_mode(dmc_vt *t, int wide)
{
	int i;
	t->requested_cols = wide ? 132 : 80;
	for (i = 0; i < t->rows; i++)
		line_blank(t->screen[i], ST_NORMAL);
	vt_set_lr_margins(t, 0);
	t->top = 0;
	t->bottom = t->rows - 1;
	t->cx = t->cy = 0;
	t->pending_wrap = 0;
}

void vt_carriage_return(dmc_vt *t)
{
	t->cx = t->cx >= t->left ? t->left : 0;
	t->pending_wrap = 0;
}

void vt_backspace(dmc_vt *t)
{
	if (t->pending_wrap)
		t->pending_wrap = 0;
	else if (t->cx > 0)
		t->cx--;
}

void vt_index(dmc_vt *t)
{
	t->pending_wrap = 0;
	if (t->cy == t->bottom) {
		if (vt_within_margins(t))
			vt_scroll_up(t, t->top, t->bottom);
	} else if (t->cy < t->rows - 1) {
		t->cy++;
	}
}

void vt_reverse_index(dmc_vt *t)
{
	t->pending_wrap = 0;
	if (t->cy == t->top) {
		if (vt_within_margins(t))
			vt_scroll_down(t, t->top, t->bottom);
	} else if (t->cy > 0) {
		t->cy--;
	}
}

/* cells left..right of from into to (a scroll between the margins) */
static void copy_cells(dmc_vt *t, struct vt_line *from, struct vt_line *to)
{
	int x;
	for (x = t->left; x <= t->right; x++) {
		const struct vt_extra *e = line_extra(from, x);
		line_set(to, x, from->ch[x], from->st[x]);
		if (e)
			extra_assign(to, x, e);
	}
}

void vt_scroll_up(dmc_vt *t, int top, int bottom)
{
	struct vt_line *gone;
	int y;
	if (!full_width(t)) {
		for (y = top; y < bottom; y++)
			copy_cells(t, t->screen[y + 1], t->screen[y]);
		line_clear(t->screen[bottom], t->left, t->right + 1, st_erased(t->style));
		return;
	}
	gone = t->screen[top];
	if (t->screen == t->main_s && top == 0 && bottom == t->rows - 1 &&
	    t->hist_max > 0) {
		struct vt_line *kept = line_new(t->cols, ST_NORMAL);
		line_copy_from(kept, gone);
		hist_push(t, kept);
	}
	for (y = top; y < bottom; y++)
		t->screen[y] = t->screen[y + 1];
	line_blank(gone, st_erased(t->style));
	t->screen[bottom] = gone;
}

void vt_scroll_down(dmc_vt *t, int top, int bottom)
{
	struct vt_line *gone;
	int y;
	if (!full_width(t)) {
		for (y = bottom; y > top; y--)
			copy_cells(t, t->screen[y - 1], t->screen[y]);
		line_clear(t->screen[top], t->left, t->right + 1, st_erased(t->style));
		return;
	}
	gone = t->screen[bottom];
	for (y = bottom; y > top; y--)
		t->screen[y] = t->screen[y - 1];
	line_blank(gone, st_erased(t->style));
	t->screen[top] = gone;
}

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

void vt_move_cursor(dmc_vt *t, int x, int y, int clamp_to_region)
{
	int lo = 0, hi = t->rows - 1, l = 0, r = t->cols - 1, w;
	if (clamp_to_region && t->cy >= t->top && t->cy <= t->bottom) {
		lo = t->top;
		hi = t->bottom;
	}
	if (vt_within_margins(t)) {
		l = t->left;
		r = t->right;
	}
	t->cy = clampi(y, lo, hi);
	w = vt_line_width(t, t->cy) - 1;
	t->cx = clampi(x, l, r < w ? r : w);
	t->pending_wrap = 0;
}

void vt_cursor_position(dmc_vt *t, int x, int y)
{
	if (t->origin_mode)
		t->cy = clampi(y + t->top, t->top, t->bottom);
	else
		t->cy = clampi(y, 0, t->rows - 1);
	if (t->origin_mode)
		t->cx = clampi(x + t->left, t->left, t->right);
	else
		t->cx = clampi(x, 0, vt_line_width(t, t->cy) - 1);
	if (t->cx > vt_line_width(t, t->cy) - 1)
		t->cx = vt_line_width(t, t->cy) - 1;
	t->pending_wrap = 0;
}

void vt_set_lr_margins(dmc_vt *t, int on)
{
	t->lr_margins = on;
	if (!on) {
		t->left = 0;
		t->right = t->cols - 1;
	}
}

void vt_set_margins(dmc_vt *t, int l, int r)
{
	int ll = clampi(l, 0, t->cols - 1), rr = clampi(r, 0, t->cols - 1);
	if (ll >= rr)
		return;
	t->left = ll;
	t->right = rr;
	vt_cursor_position(t, 0, 0);
}

void vt_set_region(dmc_vt *t, int top, int bottom)
{
	int tt = clampi(top, 0, t->rows - 1), bb = clampi(bottom, 0, t->rows - 1);
	if (tt >= bb)
		return;
	t->top = tt;
	t->bottom = bb;
	vt_cursor_position(t, 0, 0);
}

void vt_tab(dmc_vt *t, int n)
{
	int lc = vt_line_width(t, t->cy), i;
	for (i = 0; i < n; i++) {
		int x = t->cx + 1;
		while (x < lc - 1 && !t->tabs[x])
			x++;
		t->cx = x < lc - 1 ? x : lc - 1;
	}
	t->pending_wrap = 0;
}

void vt_back_tab(dmc_vt *t, int n)
{
	int i;
	for (i = 0; i < n; i++) {
		int x = t->cx - 1;
		while (x > 0 && !t->tabs[x])
			x--;
		t->cx = x > 0 ? x : 0;
	}
	t->pending_wrap = 0;
}

void vt_set_tab(dmc_vt *t)
{
	if (t->cx < t->cols)
		t->tabs[t->cx] = 1;
}

void vt_clear_tab(dmc_vt *t)
{
	if (t->cx < t->cols)
		t->tabs[t->cx] = 0;
}

void vt_clear_tabs(dmc_vt *t)
{
	memset(t->tabs, 0, (size_t)t->cols);
}

void vt_tab_stops(const dmc_vt *t, dmc_buf *out)
{
	int i, first = 1;
	char b[16];
	for (i = 0; i < t->cols; i++)
		if (t->tabs[i]) {
			snprintf(b, sizeof(b), first ? "%d" : "/%d", i + 1);
			buf_adds(out, b);
			first = 0;
		}
}

static void erase(struct vt_line *l, int from, int to, vt_style e, int selective)
{
	int x;
	if (!selective) {
		if (from <= 0 && to >= l->cols)
			line_blank(l, e);
		else
			line_clear(l, from, to, e);
		return;
	}
	for (x = from < 0 ? 0 : from; x < to && x < l->cols; x++)
		if (!(st_attrs(l->st[x]) & A_PROTECTED))
			line_clear(l, x, x + 1, e);
}

void vt_erase_display(dmc_vt *t, int mode, int selective)
{
	vt_style e = st_erased(t->style);
	int y;
	switch (mode) {
	case 0:
		erase(t->screen[t->cy], t->cx, t->cols, e, selective);
		for (y = t->cy + 1; y < t->rows; y++)
			erase(t->screen[y], 0, t->cols, e, selective);
		break;
	case 1:
		for (y = 0; y < t->cy; y++)
			erase(t->screen[y], 0, t->cols, e, selective);
		erase(t->screen[t->cy], 0, t->cx + 1, e, selective);
		break;
	case 2:
		for (y = 0; y < t->rows; y++)
			erase(t->screen[y], 0, t->cols, e, selective);
		/* ANSI.SYS homes the cursor with ESC[2J; BBSes count on it */
		if (t->home_on_clear)
			t->cx = t->cy = 0;
		break;
	case 3:
		hist_clear(t);
		break;
	}
	t->pending_wrap = 0;
}

void vt_erase_line(dmc_vt *t, int mode, int selective)
{
	vt_style e = st_erased(t->style);
	struct vt_line *l = t->screen[t->cy];
	switch (mode) {
	case 0: erase(l, t->cx, t->cols, e, selective); break;
	case 1: erase(l, 0, t->cx + 1, e, selective); break;
	case 2: erase(l, 0, t->cols, e, selective); break;
	}
	if (mode != 1)
		l->wrapped = 0;
	t->pending_wrap = 0;
}

void vt_erase_chars(dmc_vt *t, int n)
{
	line_clear(t->screen[t->cy], t->cx, t->cx + n, st_erased(t->style));
	t->pending_wrap = 0;
}

void vt_insert_chars(dmc_vt *t, int n)
{
	if (vt_within_margins(t))
		line_insert_cells(t->screen[t->cy], t->cx, n, st_erased(t->style),
				  t->right + 1);
	t->pending_wrap = 0;
}

void vt_delete_chars(dmc_vt *t, int n)
{
	if (vt_within_margins(t))
		line_delete_cells(t->screen[t->cy], t->cx, n, st_erased(t->style),
				  t->right + 1);
	t->pending_wrap = 0;
}

void vt_insert_lines(dmc_vt *t, int n)
{
	int i, k;
	if (t->cy < t->top || t->cy > t->bottom || !vt_within_margins(t))
		return;
	k = n < t->bottom - t->cy + 1 ? n : t->bottom - t->cy + 1;
	for (i = 0; i < k; i++)
		vt_scroll_down(t, t->cy, t->bottom);
	t->cx = t->left;
	t->pending_wrap = 0;
}

void vt_delete_lines(dmc_vt *t, int n)
{
	int i, k, y;
	if (t->cy < t->top || t->cy > t->bottom || !vt_within_margins(t))
		return;
	k = n < t->bottom - t->cy + 1 ? n : t->bottom - t->cy + 1;
	if (!full_width(t)) {
		for (i = 0; i < k; i++)
			vt_scroll_up(t, t->cy, t->bottom);
		t->cx = t->left;
		t->pending_wrap = 0;
		return;
	}
	for (i = 0; i < k; i++) {
		/* (not to the scrollback: the region starts at the cursor) */
		struct vt_line *gone = t->screen[t->cy];
		for (y = t->cy; y < t->bottom; y++)
			t->screen[y] = t->screen[y + 1];
		line_blank(gone, st_erased(t->style));
		t->screen[t->bottom] = gone;
	}
	t->cx = 0;
	t->pending_wrap = 0;
}

void vt_set_alt_screen(dmc_vt *t, int on, int clear)
{
	int i;
	if (on == vt_alt_screen(t))
		return;
	t->screen = on ? t->alt_s : t->main_s;
	if (on && clear)
		for (i = 0; i < t->rows; i++)
			line_blank(t->alt_s[i], ST_NORMAL);
}

void vt_save_cursor(dmc_vt *t)
{
	struct vt_saved *s = vt_alt_screen(t) ? &t->saved_alt : &t->saved_main;
	s->set = 1;
	s->x = t->cx;
	s->y = t->cy;
	s->style = t->style;
	s->wrap = t->pending_wrap;
	s->origin = t->origin_mode;
	s->have_sets = t->interp && t->interp->ops->save_sets &&
		t->interp->ops->save_sets(t->interp, &s->sets);
}

void vt_restore_cursor(dmc_vt *t)
{
	struct vt_saved *s = vt_alt_screen(t) ? &t->saved_alt : &t->saved_main;
	if (!s->set) {
		t->cx = t->cy = 0;
		t->pending_wrap = 0;
		if (t->interp->ops->restore_sets)
			t->interp->ops->restore_sets(t->interp, NULL);
		return;
	}
	t->cx = clampi(s->x, 0, t->cols - 1);
	t->cy = clampi(s->y, 0, t->rows - 1);
	t->style = s->style;
	t->pending_wrap = s->wrap;
	t->origin_mode = s->origin;
	if (t->interp->ops->restore_sets)
		t->interp->ops->restore_sets(t->interp, s->have_sets ? &s->sets : NULL);
}

void vt_alignment_test(dmc_vt *t)
{
	int y, x;
	for (y = 0; y < t->rows; y++) {
		struct vt_line *l = t->screen[y];
		for (x = 0; x < t->cols; x++)
			line_set(l, x, 'E', ST_NORMAL);
		l->size = LS_SINGLE;
		l->wrapped = 0;
	}
	t->top = 0;
	t->bottom = t->rows - 1;
	vt_cursor_position(t, 0, 0);
}

void vt_soft_reset(dmc_vt *t)
{
	t->cursor_visible = 1;
	t->app_cursor = 0;
	t->app_keypad = 0;
	t->autowrap = 1;
	t->origin_mode = 0;
	t->insert_mode = 0;
	t->top = 0;
	t->bottom = t->rows - 1;
	vt_set_lr_margins(t, 0);
	t->style = ST_NORMAL;
	if (t->interp && t->interp->ops->soft_reset)
		t->interp->ops->soft_reset(t->interp);
	t->saved_main.set = 0;
	t->saved_alt.set = 0;
}

void vt_select_page(dmc_vt *t, int n, int home)
{
	struct vt_line **next;
	if (n == t->page) {
		if (home)
			vt_cursor_position(t, 0, 0);
		return;
	}
	t->pages[t->page] = t->main_s;
	next = t->pages[n];
	t->pages[n] = NULL;
	if (!next)
		next = screen_new(t->cols, t->rows);
	if (t->screen == t->main_s)
		t->screen = next;
	t->main_s = next;
	t->page = n;
	if (home)
		vt_cursor_position(t, 0, 0);
}

static void drop_pages(dmc_vt *t)
{
	int i;
	for (i = 0; i < VT_PAGES; i++) {
		if (t->pages[i] && t->pages[i] != t->main_s)
			screen_free(t->pages[i], t->rows);
		t->pages[i] = NULL;
	}
}

static void status_clear(dmc_vt *t)
{
	line_blank(t->status.line, ST_NORMAL);
	t->status.x = 0;
}

void status_print(dmc_vt *t, int32_t cp, vt_style st)
{
	line_set(t->status.line, t->status.x, cp, st);
	if (t->status.x < t->status.line->cols - 1)
		t->status.x++;
}

void status_move(dmc_vt *t, int col)
{
	t->status.x = clampi(col, 0, t->status.line->cols - 1);
}

void status_erase(dmc_vt *t, int mode, vt_style st)
{
	struct vt_line *l = t->status.line;
	switch (mode) {
	case 0: line_clear(l, t->status.x, l->cols, st); break;
	case 1: line_clear(l, 0, t->status.x + 1, st); break;
	case 2: line_clear(l, 0, l->cols, st); break;
	}
}

void vt_status_text(const dmc_vt *t, dmc_buf *out)
{
	char b[32];
	if (t->status_type == 1) {
		snprintf(b, sizeof(b), "%04d,%03d", t->cy + 1,
			 (t->cx < t->cols - 1 ? t->cx : t->cols - 1) + 1);
		buf_adds(out, b);
	} else if (t->status_type == 2) {
		int n = out->n;
		line_text(t->status.line, 0, t->status.line->cols, out);
		if (out->n == n)
			buf_addc(out, ' ');
	}
}

void vt_use_machine(dmc_vt *t, int cols, int rows, const uint32_t *pal, int npal,
		    int fg, int bg)
{
	int i;
	t->requested_cols = cols;
	t->requested_rows = rows;
	for (i = 0; i < npal; i++)
		vt_set_color_override(t, i, pal[i]);
	vt_set_color_override(t, 256, pal[fg]);
	vt_set_color_override(t, 257, pal[bg]);
}

void vt_release_machine(dmc_vt *t)
{
	t->requested_cols = 0;
	t->requested_rows = 0;
	vt_reset_colors(t, 0, 257);
	t->style = ST_NORMAL;
}

/* ------------------------------------------------------------------ */
/* the interpreter and the profile */

void interp_free(struct vt_interp *in)
{
	if (in && in->ops->destroy)
		in->ops->destroy(in);
}

static void use_profile(dmc_vt *t, const struct vt_profile *p)
{
	if (t->interp) {
		if (t->interp->ops->detach)
			t->interp->ops->detach(t->interp);
		interp_free(t->interp);
		t->interp = NULL;
	}
	t->profile = p;
	t->host_font = NULL;
	t->charset = p->charset >= 0 ? p->charset : t->user_charset;
	t->home_on_clear = p->home_on_clear;
	t->interp = profile_interp(t, p);
	vt_soft_reset(t);
	t->changed = 1;
}

void vt_full_reset(dmc_vt *t)
{
	int i;
	vt_select_page(t, 0, 0);
	drop_pages(t);
	vt_soft_reset(t);
	t->status_type = 0;
	status_clear(t);
	t->soft_glyphs = NULL;
	t->host_font = NULL;
	t->screen = t->main_s;
	for (i = 0; i < t->rows; i++) {
		line_blank(t->main_s[i], ST_NORMAL);
		line_blank(t->alt_s[i], ST_NORMAL);
	}
	t->cx = t->cy = 0;
	t->pending_wrap = 0;
	t->bracketed_paste = 0;
	t->reverse_video = 0;
	t->newline_mode = 0;
	t->mouse_mode = MOUSE_NONE;
	t->mouse_sgr = t->mouse_urxvt = t->mouse_utf8 = 0;
	t->focus_events = 0;
	t->cursor_style = 0;
	t->requested_cols = 0;
	t->requested_rows = 0;
	t->ice_colors = 0;
	t->emulated_bps = 0;
	t->leds = 0;
	t->vt52_keys = 0;
	t->sixel_scrolling = 1;
	t->sixel_cursor_right = 0;
	vt_reset_colors(t, 0, 258);
	link_unref(t->link);
	t->link = NULL;
	t->join_next = 0;
	reset_tabs(t);
	if (t->interp->ops->reset)
		t->interp->ops->reset(t->interp);
	t->u_need = 0;
	t->changed = 1;
}

/* ------------------------------------------------------------------ */
/* the decoder */

static void decode(dmc_vt *t, int b)
{
	struct vt_interp *in = t->interp;
	if (t->charset == CS_LATIN1) {
		in->ops->process(in, b);
		return;
	}
	if (t->charset == CS_CP437) {
		in->ops->process(in, (int32_t)cp437_decode(b));
		return;
	}
	if (t->u_need > 0) {
		if ((b & 0xc0) == 0x80) {
			t->u_cp = (t->u_cp << 6) | (uint32_t)(b & 0x3f);
			if (--t->u_need == 0) {
				uint32_t c = t->u_cp;
				if (c < t->u_min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
					c = 0xfffd;
				t->interp->ops->process(t->interp, (int32_t)c);
			}
			return;
		}
		/* a sequence cut short: its replacement, then this octet anew */
		t->u_need = 0;
		in->ops->process(in, 0xfffd);
		in = t->interp;
	}
	if (b < 0x80) {
		in->ops->process(in, b);
	} else if ((b & 0xe0) == 0xc0) {
		t->u_cp = (uint32_t)(b & 0x1f);
		t->u_need = 1;
		t->u_min = 0x80;
	} else if ((b & 0xf0) == 0xe0) {
		t->u_cp = (uint32_t)(b & 0x0f);
		t->u_need = 2;
		t->u_min = 0x800;
	} else if ((b & 0xf8) == 0xf0) {
		t->u_cp = (uint32_t)(b & 0x07);
		t->u_need = 3;
		t->u_min = 0x10000;
	} else {
		in->ops->process(in, 0xfffd);
	}
}

/* ------------------------------------------------------------------ */
/* the library's calls */

dmc_vt *dmc_vt_new(int cols, int rows, int scrollback)
{
	dmc_vt *t = xcalloc(1, sizeof(*t));
	/* the CGA's colours, as the configurator was drawn for them */
	static const uint32_t cga[16] = {
		0x000000, 0xAA0000, 0x00AA00, 0xAA5500,
		0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA,
		0x555555, 0xFF5555, 0x55FF55, 0xFFFF55,
		0x5555FF, 0xFF55FF, 0x55FFFF, 0xFFFFFF,
	};
	if (cols < 1)
		cols = 1;
	if (rows < 1)
		rows = 1;
	t->cols = cols;
	t->rows = rows;
	t->hist_max = scrollback > 0 ? scrollback : 0;
	t->hist = xcalloc((size_t)(t->hist_max ? t->hist_max : 1), sizeof(*t->hist));
	t->main_s = screen_new(cols, rows);
	t->alt_s = screen_new(cols, rows);
	t->screen = t->main_s;
	t->status.line = line_new(cols, ST_NORMAL);
	t->bottom = rows - 1;
	t->right = cols - 1;
	t->autowrap = 1;
	t->cursor_visible = 1;
	t->sixel_scrolling = 1;
	t->cell_pw = 10;
	t->cell_ph = 20;
	t->style = ST_NORMAL;
	reset_tabs(t);
	t->user_charset = CS_UTF8;
	t->backspace_code = 0x08;
	/* the brand's background and text (D-Modem/brand: docs/palette.md) */
	t->theme_fg = 0xe8f0ff;
	t->theme_bg = 0x0a0f1c;
	t->theme_cursor = 0xe8f0ff;
	memcpy(t->theme_pal, cga, sizeof(cga));
	t->bold_bright = 1;
	use_profile(t, vt_profile_default());
	t->changed = 0;
	return t;
}

void dmc_vt_free(dmc_vt *t)
{
	if (!t)
		return;
	interp_free(t->interp);
	drop_pages(t);
	hist_clear(t);
	free(t->hist);
	screen_free(t->main_s, t->rows);
	screen_free(t->alt_s, t->rows);
	line_free(t->status.line);
	free(t->tabs);
	link_unref(t->link);
	free(t->music);
	buf_free(&t->reply);
	buf_free(&t->text);
	buf_free(&t->title);
	buf_free(&t->print);
	free(t);
}

void dmc_vt_reset(dmc_vt *t)
{
	vt_full_reset(t);
	hist_clear(t);
	buf_clear(&t->reply);
}

/* a screen's lines at the new size: lines dropped from the bottom (blank
   ones) then the top (to the scrollback, the main's), more taken back
   from it; the cursor kept on its line when active */
static struct vt_line **resize_screen(dmc_vt *t, struct vt_line **old, int ncols,
				      int nrows, int is_main, int active)
{
	int n = t->rows, cy = t->cy, i, first = 0;
	struct vt_line **list = xcalloc((size_t)(n + nrows + t->hist_n + 1),
					sizeof(*list));
	struct vt_line **out;
	int len;
	for (i = 0; i < n; i++) {
		line_resize(old[i], ncols);
		list[i] = old[i];
	}
	len = n;
	if (nrows < len) {
		int excess = len - nrows;
		while (excess > 0 && len - 1 > (active ? cy : -1)) {
			struct vt_line *last = list[first + len - 1];
			dmc_buf b = { 0 };
			int empty;
			line_text(last, 0, last->cols, &b);
			empty = b.n == 0;
			buf_free(&b);
			if (!empty)
				break;
			line_free(last);
			len--;
			excess--;
		}
		while (excess > 0) {
			struct vt_line *l = list[first];
			first++;
			len--;
			if (is_main && t->hist_max > 0)
				hist_push(t, l);
			else
				line_free(l);
			if (active)
				cy--;
			excess--;
		}
	}
	out = xcalloc((size_t)nrows, sizeof(*out));
	if (nrows > len) {
		int more = nrows - len, k = 0, j;
		struct vt_line *back[1024];
		int nb = 0;
		while (more > 0 && is_main && t->hist_n > 0 && active && nb < 1024) {
			back[nb++] = hist_pop(t);
			cy++;
			more--;
		}
		for (j = nb - 1; j >= 0; j--) {
			line_resize(back[j], ncols);
			out[k++] = back[j];
		}
		for (j = 0; j < len; j++)
			out[k++] = list[first + j];
		while (k < nrows)
			out[k++] = line_new(ncols, ST_NORMAL);
	} else {
		for (i = 0; i < nrows; i++)
			out[i] = list[first + i];
	}
	if (active)
		t->cy = clampi(cy, 0, nrows - 1);
	free(list);
	free(old);
	return out;
}

void dmc_vt_resize(dmc_vt *t, int cols, int rows)
{
	int was_alt, i;
	if (cols < 1 || rows < 1 || (cols == t->cols && rows == t->rows))
		return;
	was_alt = vt_alt_screen(t);
	drop_pages(t);
	for (i = 0; i < t->hist_n; i++)
		line_resize(hist_at(t, i), cols);
	t->main_s = resize_screen(t, t->main_s, cols, rows, 1, !was_alt);
	t->alt_s = resize_screen(t, t->alt_s, cols, rows, 0, was_alt);
	t->screen = was_alt ? t->alt_s : t->main_s;
	t->cols = cols;
	t->rows = rows;
	line_resize(t->status.line, cols);
	t->status.x = clampi(t->status.x, 0, cols - 1);
	t->lr_margins = 0;
	t->left = 0;
	t->right = cols - 1;
	t->top = 0;
	t->bottom = rows - 1;
	reset_tabs(t);
	t->cx = clampi(t->cx, 0, cols - 1);
	t->pending_wrap = 0;
	t->changed = 1;
}

int dmc_vt_cols(const dmc_vt *t) { return t->cols; }
int dmc_vt_rows(const dmc_vt *t) { return t->rows; }

void dmc_vt_feed(dmc_vt *t, const char *data, int n)
{
	int i;
	for (i = 0; i < n; i++) {
		int b = (unsigned char)data[i];
		if (t->interp->reads_octets)
			t->interp->ops->process(t->interp, b);
		else
			decode(t, b);
	}
	if (t->interp->ops->flush)
		t->interp->ops->flush(t->interp);
	t->changed = 1;
}

int dmc_vt_reply(dmc_vt *t, char *buf, int max)
{
	return buf_take(&t->reply, buf, max);
}

int dmc_vt_history(const dmc_vt *t)
{
	return history_size(t);
}

/* the line at row (NULL off the screen and scrollback) */
static const struct vt_line *row_line(const dmc_vt *t, int row)
{
	if (row >= t->rows || row < -history_size(t))
		return NULL;
	return vt_line(t, row);
}

uint32_t dmc_vt_char(const dmc_vt *t, int row, int col)
{
	const struct vt_line *l = row_line(t, row);
	int32_t c;
	if (!l || col < 0 || col >= l->cols)
		return ' ';
	c = l->ch[col];
	return c == 0 ? ' ' : c == WIDE_TAIL ? 0 : (uint32_t)c;
}

static int index_color(int c, int def)
{
	if (c == ST_DEFAULT_FG || c == ST_DEFAULT_BG || (c & ST_RGB))
		return def;
	return c;
}

int dmc_vt_fg(const dmc_vt *t, int row, int col)
{
	const struct vt_line *l = row_line(t, row);
	if (!l || col < 0 || col >= l->cols)
		return -1;
	return index_color(st_fg(l->st[col]), -1);
}

int dmc_vt_bg(const dmc_vt *t, int row, int col)
{
	const struct vt_line *l = row_line(t, row);
	if (!l || col < 0 || col >= l->cols)
		return -1;
	return index_color(st_bg(l->st[col]), -1);
}

int dmc_vt_attr(const dmc_vt *t, int row, int col)
{
	const struct vt_line *l = row_line(t, row);
	int a, r = 0;
	if (!l || col < 0 || col >= l->cols)
		return 0;
	a = st_attrs(l->st[col]);
	if (a & A_BOLD)
		r |= DMC_BOLD;
	if (a & A_ANY_UNDER)
		r |= DMC_UNDERLINE;
	if (a & A_INVERSE)
		r |= DMC_REVERSE;
	return r;
}

int dmc_vt_cell(const dmc_vt *t, int row, int col, dmc_cell *c)
{
	const struct vt_line *l = row_line(t, row);
	const struct vt_extra *e;
	int a, fg, bg, flags = 0;
	uint32_t f, b;
	int32_t ch;
	memset(c, 0, sizeof(*c));
	c->fg = t->theme_fg;
	c->bg = t->theme_bg;
	if (!l || col < 0 || col >= l->cols) {
		c->ch = ' ';
		return 0;
	}
	ch = l->ch[col];
	a = st_attrs(l->st[col]);
	fg = st_fg(l->st[col]);
	bg = st_bg(l->st[col]);
	if ((a & A_BOLD) && t->bold_bright && fg >= 0 && fg < 8)
		fg += 8;
	if (t->ice_colors && (a & A_BLINK) && bg >= 0 && bg < 8)
		bg += 8;
	f = resolve(t, fg, 1);
	b = resolve(t, bg, 0);
	if (((a & A_INVERSE) != 0) != (t->reverse_video != 0)) {
		uint32_t x = f;
		f = b;
		b = x;
	}
	if (a & A_FAINT)
		f = ((f >> 1) & 0x7f7f7f) + ((b >> 1) & 0x7f7f7f);
	c->fg = f & 0xffffff;
	c->bg = b & 0xffffff;
	if ((a & A_BOLD) && !(t->bold_bright && st_fg(l->st[col]) >= 0 &&
			      st_fg(l->st[col]) < 8))
		flags |= DMC_CELL_BOLD;
	if (a & A_ITALIC)
		flags |= DMC_CELL_ITALIC;
	if (a & A_UNDERLINE)
		flags |= DMC_CELL_UNDERLINE;
	if (a & A_DUNDER)
		flags |= DMC_CELL_DOUBLE_UNDERLINE;
	if (a & A_CURLY)
		flags |= DMC_CELL_CURLY_UNDERLINE;
	if (a & A_STRIKE)
		flags |= DMC_CELL_STRIKE;
	if (a & A_OVERLINE)
		flags |= DMC_CELL_OVERLINE;
	if ((a & A_BLINK) && !t->ice_colors)
		flags |= DMC_CELL_BLINK;
	if (a & A_INVISIBLE)
		flags |= DMC_CELL_INVISIBLE;
	if (ch == WIDE_TAIL) {
		flags |= DMC_CELL_WIDE_TAIL;
		c->ch = 0;
	} else {
		c->ch = ch == 0 ? ' ' : (uint32_t)ch;
		if (ch > 0 && col + 1 < l->cols && l->ch[col + 1] == WIDE_TAIL)
			flags |= DMC_CELL_WIDE;
	}
	e = line_extra(l, col);
	if (e && e->img)
		flags |= DMC_CELL_IMAGE;
	if (e && e->ncomb)
		flags |= DMC_CELL_COMBINING;
	if (e && e->link)
		flags |= DMC_CELL_LINK;
	if (ch > 0 && dmc_vt_glyph(t, (uint32_t)ch, NULL, NULL))
		flags |= DMC_CELL_GLYPH;
	c->flags = flags;
	return 1;
}

const char *dmc_vt_cell_text(dmc_vt *t, int row, int col)
{
	const struct vt_line *l = row_line(t, row);
	const struct vt_extra *e;
	int k;
	buf_clear(&t->text);
	if (!l || col < 0 || col >= l->cols || l->ch[col] == WIDE_TAIL)
		return buf_str(&t->text);
	utf8_put(&t->text, l->ch[col] == 0 ? ' ' : (uint32_t)l->ch[col]);
	e = line_extra(l, col);
	if (e)
		for (k = 0; k < e->ncomb; k++)
			utf8_put(&t->text, e->comb[k]);
	return buf_str(&t->text);
}

const char *dmc_vt_cell_link(const dmc_vt *t, int row, int col)
{
	const struct vt_line *l = row_line(t, row);
	const struct vt_extra *e;
	if (!l)
		return NULL;
	e = line_extra(l, col);
	return e && e->link ? e->link->uri : NULL;
}

const uint32_t *dmc_vt_cell_image(const dmc_vt *t, int row, int col,
				   dmc_image *im)
{
	const struct vt_line *l = row_line(t, row);
	const struct vt_extra *e;
	if (!l)
		return NULL;
	e = line_extra(l, col);
	if (!e || !e->img)
		return NULL;
	if (im) {
		im->id = e->img->id;
		im->width = e->img->width;
		im->height = e->img->height;
		im->cell_width = e->img->cell_w;
		im->cell_height = e->img->cell_h;
		im->tx = e->tx;
		im->ty = e->ty;
	}
	return e->img->pixels;
}

int dmc_vt_line_size(const dmc_vt *t, int row)
{
	const struct vt_line *l = row_line(t, row);
	return l ? l->size : 0;
}

const char *dmc_vt_text(dmc_vt *t, int row)
{
	const struct vt_line *l = row_line(t, row);
	buf_clear(&t->text);
	if (l)
		line_text(l, 0, l->cols, &t->text);
	/* (the trailing blanks a space wrote dropped too) */
	while (t->text.n > 0 && t->text.p[t->text.n - 1] == ' ')
		buf_pop(&t->text);
	return buf_str(&t->text);
}

const char *dmc_vt_text_range(dmc_vt *t, int row, int col, int row2, int col2)
{
	int r;
	buf_clear(&t->text);
	for (r = row; r <= row2; r++) {
		const struct vt_line *l = row_line(t, r);
		int from, to;
		if (!l)
			continue;
		from = r == row ? clampi(col, 0, l->cols) : 0;
		to = r == row2 ? clampi(col2 + 1, 0, l->cols) : l->cols;
		line_text(l, from, to, &t->text);
		if (r != row2 && !l->wrapped)
			buf_addc(&t->text, '\n');
	}
	return buf_str(&t->text);
}

int dmc_vt_cursor_row(const dmc_vt *t) { return t->cy; }
int dmc_vt_cursor_col(const dmc_vt *t)
{
	return t->cx < t->cols - 1 ? t->cx : t->cols - 1;
}
int dmc_vt_cursor_visible(const dmc_vt *t) { return t->cursor_visible; }
int dmc_vt_cursor_style(const dmc_vt *t) { return t->cursor_style; }

int dmc_vt_changed(dmc_vt *t)
{
	int c = t->changed;
	t->changed = 0;
	return c;
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

uint32_t dmc_vt_background(const dmc_vt *t)
{
	return vt_shown_color(t, 257) & 0xffffff;
}

uint32_t dmc_vt_foreground(const dmc_vt *t)
{
	return vt_shown_color(t, 256) & 0xffffff;
}

uint32_t dmc_vt_cursor_color(const dmc_vt *t)
{
	return vt_shown_color(t, 258) & 0xffffff;
}

void dmc_vt_set_theme(dmc_vt *t, uint32_t fg, uint32_t bg, uint32_t cursor,
		      const uint32_t *palette16)
{
	t->theme_fg = fg & 0xffffff;
	t->theme_bg = bg & 0xffffff;
	t->theme_cursor = cursor & 0xffffff;
	if (palette16)
		memcpy(t->theme_pal, palette16, sizeof(t->theme_pal));
	t->changed = 1;
}

void dmc_vt_set_bold_bright(dmc_vt *t, int on)
{
	t->bold_bright = on != 0;
	t->changed = 1;
}

/* the profiles */

int dmc_vt_profile_count(void)
{
	return vt_profile_count;
}

const char *dmc_vt_profile_id(int i)
{
	return i >= 0 && i < vt_profile_count ? vt_profiles[i]->id : NULL;
}

const char *dmc_vt_profile_label(int i)
{
	return i >= 0 && i < vt_profile_count ? vt_profiles[i]->label : NULL;
}

const char *dmc_vt_profile_term(const char *id)
{
	const struct vt_profile *p = vt_profile_by_id(id);
	return (p ? p : vt_profile_default())->term;
}

const char *dmc_vt_profile_telnet(const char *id)
{
	const struct vt_profile *p = vt_profile_by_id(id);
	return (p ? p : vt_profile_default())->telnet;
}

int dmc_vt_set_profile(dmc_vt *t, const char *id)
{
	const struct vt_profile *p = vt_profile_by_id(id);
	if (!p)
		return 0;
	if (p != t->profile)
		use_profile(t, p);
	return 1;
}

const char *dmc_vt_get_profile(const dmc_vt *t)
{
	return t->profile->id;
}

void dmc_vt_set_charset(dmc_vt *t, int charset)
{
	if (charset < DMC_CHARSET_UTF8 || charset > DMC_CHARSET_LATIN1)
		return;
	t->user_charset = charset;
	if (t->profile->charset < 0)
		t->charset = charset;
	t->u_need = 0;
}

int dmc_vt_requested_cols(const dmc_vt *t) { return t->requested_cols; }
int dmc_vt_requested_rows(const dmc_vt *t) { return t->requested_rows; }

void dmc_vt_set_cell_pixels(dmc_vt *t, int w, int h)
{
	t->cell_pw = w > 1 ? w : 1;
	t->cell_ph = h > 1 ? h : 1;
}

const struct vt_font *vt_glyphs(const dmc_vt *t)
{
	return t->host_font ? t->host_font : machine_font(t->profile->font);
}

int dmc_vt_glyph(const dmc_vt *t, uint32_t cp, int *w, int *h)
{
	const struct vt_font *f = NULL, *g = vt_glyphs(t);
	if (t->soft_glyphs && font_index(t->soft_glyphs, cp) >= 0)
		f = t->soft_glyphs;
	else if (g && font_index(g, cp) >= 0)
		f = g;
	if (!f)
		return 0;
	if (w)
		*w = f->w;
	if (h)
		*h = f->h;
	return 1;
}

uint32_t dmc_vt_glyph_row(const dmc_vt *t, uint32_t cp, int row)
{
	if (t->soft_glyphs && font_index(t->soft_glyphs, cp) >= 0)
		return font_row(t->soft_glyphs, cp, row);
	if (vt_glyphs(t))
		return font_row(vt_glyphs(t), cp, row);
	return 0;
}

int dmc_vt_glyph_cell(const dmc_vt *t, int *w, int *h)
{
	const struct vt_font *f = vt_glyphs(t);
	if (!f)
		return 0;
	if (w)
		*w = f->w;
	if (h)
		*h = f->h;
	return 1;
}

const char *dmc_vt_status_line(dmc_vt *t)
{
	buf_clear(&t->text);
	vt_status_text(t, &t->text);
	return buf_str(&t->text);
}

const char *dmc_vt_title(dmc_vt *t)
{
	t->title_changed = 0;
	return buf_str(&t->title);
}

int dmc_vt_title_changed(const dmc_vt *t)
{
	return t->title_changed;
}

int dmc_vt_bells(dmc_vt *t)
{
	int n = t->bells;
	t->bells = 0;
	return n;
}

int dmc_vt_music(dmc_vt *t, uint32_t *hz100, int *ms, int *rest_ms)
{
	if (t->music_n < 3)
		return 0;
	*hz100 = t->music[0];
	*ms = (int)t->music[1];
	*rest_ms = (int)t->music[2];
	memmove(t->music, t->music + 3, (size_t)(t->music_n - 3) * sizeof(uint32_t));
	t->music_n -= 3;
	return 1;
}

int dmc_vt_printed(dmc_vt *t, char *buf, int max)
{
	return buf_take(&t->print, buf, max);
}

int dmc_vt_emulated_bps(const dmc_vt *t)
{
	return t->emulated_bps;
}

/* ------------------------------------------------------------------ */
/* keys */

static int take(dmc_buf *b, char *buf, int max)
{
	int n = b->n;
	if (n > max) {
		buf_free(b);
		return 0;
	}
	if (n)
		memcpy(buf, b->p, (size_t)n);
	buf_free(b);
	return n;
}

int dmc_vt_key_mods(dmc_vt *t, int key, int mods, char *buf, int max)
{
	dmc_buf b = { 0 };
	int m = 0;
	if (mods & DMC_MOD_SHIFT)
		m |= M_SHIFT;
	if (mods & DMC_MOD_ALT)
		m |= M_ALT;
	if (mods & DMC_MOD_CTRL)
		m |= M_CTRL;
	if (key == DMC_KEY_BACKTAB) {
		key = DMC_KEY_TAB;
		m |= M_SHIFT;
	}
	if (!(t->interp->ops->key && t->interp->ops->key(t->interp, key, m, &b))) {
		dmc_buf s = { 0 };
		int i, n;
		uint32_t cps[64];
		keys_key(t, key, m, &s);
		/* the VT sequence in the character set (they're ASCII but for
		   Enter's own) */
		n = 0;
		for (i = 0; i < s.n && n < 64; i++)
			cps[n++] = (unsigned char)s.p[i];
		charset_encode(t->charset, cps, n, &b);
		buf_free(&s);
	}
	return take(&b, buf, max);
}

int dmc_vt_key(const dmc_vt *t, int key, char *buf, int max)
{
	return dmc_vt_key_mods((dmc_vt *)t, key, 0, buf, max);
}

int dmc_vt_char_bytes(dmc_vt *t, uint32_t cp, int mods, char *buf, int max)
{
	dmc_buf b = { 0 };
	int m = 0;
	if (mods & DMC_MOD_SHIFT)
		m |= M_SHIFT;
	if (mods & DMC_MOD_ALT)
		m |= M_ALT;
	if (mods & DMC_MOD_CTRL)
		m |= M_CTRL;
	if (cp == '\r' || cp == '\n')
		return dmc_vt_key_mods(t, DMC_KEY_ENTER, mods & DMC_MOD_ALT, buf, max);
	if (!(t->interp->ops->chr && t->interp->ops->chr(t->interp, cp, m, &b))) {
		dmc_buf s = { 0 };
		uint32_t cps[8];
		int n = 0;
		keys_char(cp, m, &s);
		/* (keys_char gives UTF-8: decoded back to code points) */
		{
			int i = 0;
			while (i < s.n && n < 8) {
				unsigned char c = (unsigned char)s.p[i];
				uint32_t v;
				int k;
				if (c < 0x80) {
					v = c;
					k = 1;
				} else if ((c & 0xe0) == 0xc0) {
					v = c & 0x1f;
					k = 2;
				} else if ((c & 0xf0) == 0xe0) {
					v = c & 0x0f;
					k = 3;
				} else {
					v = c & 0x07;
					k = 4;
				}
				for (i++; --k > 0 && i < s.n; i++)
					v = (v << 6) | ((unsigned char)s.p[i] & 0x3f);
				cps[n++] = v;
			}
		}
		charset_encode(t->charset, cps, n, &b);
		buf_free(&s);
	}
	return take(&b, buf, max);
}

void dmc_vt_set_enter(dmc_vt *t, int mode)
{
	t->enter_mode = mode >= 0 && mode <= 2 ? mode : 0;
}

void dmc_vt_set_backspace(dmc_vt *t, int code)
{
	t->backspace_code = code == 0x7f ? 0x7f : 0x08;
}

int dmc_vt_mouse(dmc_vt *t, int button, int col, int row, int press, int motion,
		 int mods, char *buf, int max)
{
	int mode = t->mouse_mode, wheel = button >= 64, code, x, y, m = 0;
	char s[64];
	if (mode == MOUSE_NONE)
		return 0;
	if (motion && mode != MOUSE_BUTTON && mode != MOUSE_ANY)
		return 0;
	if (!press && (mode == MOUSE_X10 || wheel))
		return 0;
	if (mods & DMC_MOD_SHIFT)
		m |= 4;
	if (mods & DMC_MOD_ALT)
		m |= 8;
	if (mods & DMC_MOD_CTRL)
		m |= 16;
	code = button + (motion ? 32 : 0) + (mode == MOUSE_X10 ? 0 : m);
	x = clampi(col, 0, t->cols - 1) + 1;
	y = clampi(row, 0, t->rows - 1) + 1;
	if (t->mouse_sgr) {
		snprintf(s, sizeof(s), "\x1b[<%d;%d;%d%c", code, x, y, press ? 'M' : 'm');
	} else if (t->mouse_urxvt) {
		if (!press)
			code = 3 + (motion ? 32 : 0) + m;
		snprintf(s, sizeof(s), "\x1b[%d;%d;%dM", code + 32, x, y);
	} else {
		dmc_buf b = { 0 };
		if (!press)
			code = 3 + (motion ? 32 : 0) + m;
		if (!t->mouse_utf8 && (x > 223 || y > 223))
			return 0;
		buf_adds(&b, "\x1b[M");
		if (t->mouse_utf8) {
			utf8_put(&b, (uint32_t)(32 + code));
			utf8_put(&b, (uint32_t)(32 + x));
			utf8_put(&b, (uint32_t)(32 + y));
		} else {
			buf_addc(&b, (char)(32 + code));
			buf_addc(&b, (char)(32 + x));
			buf_addc(&b, (char)(32 + y));
		}
		return take(&b, buf, max);
	}
	{
		int n = (int)strlen(s);
		if (n > max)
			return 0;
		memcpy(buf, s, (size_t)n);
		return n;
	}
}

int dmc_vt_mouse_wanted(const dmc_vt *t)
{
	return t->mouse_mode != MOUSE_NONE;
}

int dmc_vt_bracketed_paste(const dmc_vt *t)
{
	return t->bracketed_paste;
}

int dmc_vt_focus(dmc_vt *t, int focused, char *buf, int max)
{
	if (!t->focus_events || max < 3)
		return 0;
	memcpy(buf, focused ? "\x1b[I" : "\x1b[O", 3);
	return 3;
}

int dmc_vt_tap(dmc_vt *t, double fx, double fy, char *buf, int max)
{
	dmc_buf b = { 0 };
	if (!t->interp->ops->tap || !t->interp->ops->tap(t->interp, fx, fy, &b)) {
		buf_free(&b);
		return 0;
	}
	return take(&b, buf, max);
}

void dmc_vt_type_text(dmc_vt *t, const char *utf8, int n)
{
	/* (local echo, the tests: as the remote's text) */
	dmc_vt_feed(t, utf8, n);
}
