/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * vt_rip.c - RIPscrip 1.54 (the app's RipInterpreter), TeleGrafix's
 * graphics for BBSes: ANSI-BBS whose lines that start with !| are
 * drawing commands on a 640x350 EGA screen (lines, rectangles, circles,
 * ellipses, arcs, pies, Béziers, polygons, flood and patterned fills,
 * text, the clipboard) and mouse regions and buttons that a tap sends
 * the host.  Ordinary text and ANSI go back to ANSI-BBS's parser (vt_dec.c)
 * for the text window, 80x43 of 8x8 cells.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "vtint.h"

#define WIDTH 640
#define HEIGHT 350
#define ASPECT 0.775
#define MAX_LINE (1 << 16)

/* a run of code points */
struct cps {
	uint32_t *v;
	int n, cap;
};

static void cps_add(struct cps *s, uint32_t c)
{
	if (s->n == s->cap) {
		int cap = s->cap ? s->cap * 2 : 64;
		uint32_t *v = (uint32_t *)realloc(s->v, sizeof(uint32_t) * (size_t)cap);
		if (!v)
			return;
		s->v = v;
		s->cap = cap;
	}
	s->v[s->n++] = c;
}

struct region {
	int x0, y0, x1, y1;
	uint32_t *send;
	int n;
};

struct rip {
	dmc_vt *t;
	void (*replay)(void *, int32_t);
	void *ctx;
	struct picture_layer layer;

	int line_start, bang, in_line, escaped, swallow_lf, replaying, enabled;
	struct cps line;
	int32_t held[8];
	int held_n;

	int color, x, y, xor_, line_pattern, thick, fill_pattern;
	uint8_t user_fill[8];
	int fill_color, font, font_size, vertical;
	uint32_t palette[16];
	int viewport[4], has_viewport;
	struct raster_grab clipboard;
	int has_clipboard;

	struct region *regions;
	int nregions, cap_regions;

	int bevel, b_fore, b_surface, b_bright, b_dark;
	int block[5], has_block;
};

/* an EGA colour (0-63: rgbRGB, the lower case the 1/3 bits) as ARGB */
static uint32_t ega(int v)
{
#define CH(hi, lo) ((((v) >> (hi)) & 1 ? 0xaa : 0) + (((v) >> (lo)) & 1 ? 0x55 : 0))
	return 0xff000000u | ((uint32_t)CH(2, 5) << 16) |
	       ((uint32_t)CH(1, 4) << 8) | (uint32_t)CH(0, 3);
#undef CH
}

static const int ega_start[16] = {
	0, 1, 2, 3, 4, 5, 20, 7, 56, 57, 58, 59, 60, 61, 62, 63
};

/* BGI's fill patterns 2-11 */
static const uint8_t fills[10][8] = {
	{ 0xff, 0xff, 0, 0, 0xff, 0xff, 0, 0 },
	{ 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80 },
	{ 0xe0, 0xc1, 0x83, 0x07, 0x0e, 0x1c, 0x38, 0x70 },
	{ 0xf0, 0x78, 0x3c, 0x1e, 0x0f, 0x87, 0xc3, 0xe1 },
	{ 0xa5, 0xd2, 0x69, 0xb4, 0x5a, 0x2d, 0x96, 0x4b },
	{ 0xff, 0x88, 0x88, 0x88, 0xff, 0x88, 0x88, 0x88 },
	{ 0x81, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x81 },
	{ 0xcc, 0x33, 0xcc, 0x33, 0xcc, 0x33, 0xcc, 0x33 },
	{ 0x80, 0x00, 0x08, 0x00, 0x80, 0x00, 0x08, 0x00 },
	{ 0x88, 0x00, 0x22, 0x00, 0x88, 0x00, 0x22, 0x00 },
};
static const uint8_t hollow[8];

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static int roundi(double v)
{
	return (int)floor(v + 0.5);
}

static struct raster *R(struct rip *r)
{
	return layer_raster(&r->layer);
}

/* ------------------------------------------------------------------ */
/* a command's arguments: MegaNums (base 36), then the text left */

struct args {
	const uint32_t *s;
	int n, i;
};

static int arg_w(struct args *a, int width)
{
	int v = 0;
	while (width-- > 0) {
		uint32_t c = a->i < a->n ? a->s[a->i] : '0';
		a->i++;
		v *= 36;
		if (c >= '0' && c <= '9')
			v += (int)(c - '0');
		else if (c >= 'A' && c <= 'Z')
			v += (int)(c - 'A') + 10;
		else if (c >= 'a' && c <= 'z')
			v += (int)(c - 'a') + 10;
	}
	return v;
}

static int arg(struct args *a)
{
	return arg_w(a, 2);
}

static const uint32_t *rest(struct args *a, int *n)
{
	*n = a->i < a->n ? a->n - a->i : 0;
	return a->i < a->n ? a->s + a->i : a->s;
}

/* ------------------------------------------------------------------ */
/* drawing */

/* colour register c as drawn: black transparent (the text shows through) */
static uint32_t argb(struct rip *r, int c)
{
	uint32_t v = r->palette[c & 15];
	return (v & 0xffffff) == 0 ? 0 : v;
}

static void set_up(struct rip *r)
{
	struct raster *g = R(r);
	if (!g)
		return;
	g->mode = r->xor_ ? R_COMPLEMENT : R_OVERLAY;
	g->color = argb(r, r->color);
	g->pattern = r->line_pattern;
	g->fill_pattern = NULL;
	g->has_clip = r->has_viewport;
	if (r->has_viewport)
		raster_pixel_rect(g, r->viewport[0], r->viewport[1],
				  r->viewport[2], r->viewport[3], g->clip);
	if (g->color == 0)
		g->mode = R_ERASE;
	g->background = 0;
}

static void set_up_fill(struct rip *r)
{
	struct raster *g;
	set_up(r);
	g = R(r);
	if (!g)
		return;
	g->color = argb(r, r->fill_color);
	g->mode = g->color == 0 ? R_ERASE : R_OVERLAY;
	switch (r->fill_pattern) {
	case 0: g->fill_pattern = hollow; break;
	case 1: g->fill_pattern = NULL; break;
	case 12:
		memcpy(g->fill_pattern_buf, r->user_fill, 8);
		g->fill_pattern = g->fill_pattern_buf;
		break;
	default:
		g->fill_pattern = r->fill_pattern >= 2 && r->fill_pattern <= 11
			? fills[r->fill_pattern - 2] : NULL;
	}
	g->fill_background = 0;
}

static void line(struct rip *r, int x0, int y0, int x1, int y1)
{
	struct raster *g;
	set_up(r);
	g = R(r);
	if (!g)
		return;
	raster_line(g, x0, y0, x1, y1);
	if (r->thick == 3) {
		raster_line(g, x0 + 1, y0, x1 + 1, y1);
		raster_line(g, x0, y0 + 1, x1, y1 + 1);
	}
}

/* lines through n points (xy pairs) */
static void polyline(struct rip *r, const int *xy, int n)
{
	int i;
	for (i = 1; i < n; i++)
		line(r, xy[2 * i - 2], xy[2 * i - 1], xy[2 * i], xy[2 * i + 1]);
}

static void fill(struct rip *r, const int *xy, int n)
{
	set_up_fill(r);
	if (R(r))
		raster_fill(R(r), xy, n);
}

static void box_fill(struct rip *r, int x0, int y0, int x1, int y1)
{
	int q[8];
	q[0] = x0; q[1] = y0;
	q[2] = x1 + 1; q[3] = y0;
	q[4] = x1 + 1; q[5] = y1 + 1;
	q[6] = x0; q[7] = y1 + 1;
	fill(r, q, 4);
}

static double sweep(int s, int e)
{
	int d = ((e - s) % 360 + 360) % 360;
	return d == 0 ? 360.0 : (double)d;
}

static int aspect(int rad)
{
	return roundi(rad * ASPECT);
}

static void arc(struct rip *r, int cx, int cy, int s, int e, int rx, int ry)
{
	set_up(r);
	if (R(r))
		raster_ellipse(R(r), cx, cy, rx, ry, (double)s, sweep(s, e));
}

#define MAX_ARC 80

static void pie(struct rip *r, int cx, int cy, int s, int e, int rx, int ry)
{
	int xy[2 * (MAX_ARC + 2)], n;
	xy[0] = cx;
	xy[1] = cy;
	n = 1 + raster_ellipse_points(cx, cy, rx, ry, (double)s, sweep(s, e),
				      xy + 2, MAX_ARC);
	xy[2 * n] = cx;
	xy[2 * n + 1] = cy;
	n++;
	fill(r, xy, n);
	polyline(r, xy, n);
}

static void bezier(struct rip *r, const int *p, int n)
{
	int *xy = (int *)malloc(sizeof(int) * 2 * (size_t)(n + 1)), i;
	if (!xy)
		return;
	for (i = 0; i <= n; i++) {
		double u = (double)i / n, v = 1 - u;
		double a = v * v * v, b = 3 * v * v * u, c = 3 * v * u * u,
		       d = u * u * u;
		xy[2 * i] = roundi(a * p[0] + b * p[2] + c * p[4] + d * p[6]);
		xy[2 * i + 1] = roundi(a * p[1] + b * p[3] + c * p[5] + d * p[7]);
	}
	polyline(r, xy, n + 1);
	free(xy);
}

static int cell_height(struct rip *r)
{
	return r->font == 0 ? 8 * r->font_size : 10 * r->font_size;
}

/* text drawn from the position in RIPterm's 8x8 font (bigger for the
   stroke fonts' sizes), the position left after it */
static void text(struct rip *r, const uint32_t *s, int n)
{
	struct raster *g;
	const struct vt_font *f = machine_font(MF_RIPTERM);
	int cw = r->font == 0 ? 8 * r->font_size : 6 * r->font_size;
	int ch = cell_height(r), i;
	set_up(r);
	g = R(r);
	if (!g)
		return;
	g->mode = r->xor_ ? R_COMPLEMENT : R_OVERLAY;
	for (i = 0; i < n; i++) {
		raster_text(g, r->x, r->y, s + i, 1, f, cw, ch);
		if (r->vertical)
			r->y += ch;
		else
			r->x += cw;
	}
}

/* ------------------------------------------------------------------ */
/* windows and regions */

static void add_region(struct rip *r, int x0, int y0, int x1, int y1,
		       const uint32_t *s, int n)
{
	struct region *g;
	if (r->nregions == r->cap_regions) {
		int cap = r->cap_regions ? r->cap_regions * 2 : 16;
		struct region *v = (struct region *)realloc(
			r->regions, sizeof(*v) * (size_t)cap);
		if (!v)
			return;
		r->regions = v;
		r->cap_regions = cap;
	}
	g = &r->regions[r->nregions];
	g->send = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)(n ? n : 1));
	if (!g->send)
		return;
	memcpy(g->send, s, sizeof(uint32_t) * (size_t)n);
	g->n = n;
	g->x0 = x0;
	g->y0 = y0;
	g->x1 = x1;
	g->y1 = y1;
	r->nregions++;
}

static void clear_regions(struct rip *r)
{
	int i;
	for (i = 0; i < r->nregions; i++)
		free(r->regions[i].send);
	r->nregions = 0;
}

static void drop_clipboard(struct rip *r)
{
	if (r->has_clipboard)
		free(r->clipboard.px);
	r->has_clipboard = 0;
	memset(&r->clipboard, 0, sizeof(r->clipboard));
}

static void text_window(struct rip *r, int x0, int y0, int x1, int y1)
{
	dmc_vt *t = r->t;
	int top, bottom;
	(void)x1;
	if (!x0 && !y0 && !x1 && !y1)
		return;
	top = clampi(y0, 0, t->rows - 1);
	bottom = clampi(y1, top, t->rows - 1);
	vt_set_region(t, top, bottom);
	vt_move_cursor(t, clampi(x0, 0, t->cols - 1), top, 0);
}

/* RIP_RESET_WINDOWS: the picture, text, windows, palette and regions as
   at the start */
static void reset_windows(struct rip *r)
{
	dmc_vt *t = r->t;
	int i;
	layer_reset(&r->layer);
	vt_set_region(t, 0, t->rows - 1);
	vt_erase_display(t, 2, 0);
	vt_move_cursor(t, 0, 0, 0);
	for (i = 0; i < 16; i++)
		r->palette[i] = ega(ega_start[i]);
	r->has_viewport = 0;
	clear_regions(r);
	drop_clipboard(r);
	r->color = 15;
	r->x = r->y = 0;
}

/* a button (1U): its bevel, surface and label in the buttons' style, and
   a region sending its host text ("icon<>label<>host") */
static void button(struct rip *r, int x0, int y0, int x1, int y1,
		   const uint32_t *s, int n)
{
	const uint32_t *part[3] = { NULL, NULL, NULL };
	int plen[3] = { 0, 0, 0 }, k = 0, i, start = 0, b;
	int sc, sp, scolor, sfont, ssize, svert, q[6];
	for (i = 0; i <= n && k < 3; i++) {
		if (i == n || (s[i] == '<' && i + 1 < n && s[i + 1] == '>')) {
			part[k] = s + start;
			plen[k] = i - start;
			k++;
			start = i + 2;
			i++;
		}
	}
	sc = r->fill_color;
	sp = r->fill_pattern;
	r->fill_color = r->b_surface;
	r->fill_pattern = 1;
	box_fill(r, x0, y0, x1, y1);
	r->fill_color = sc;
	r->fill_pattern = sp;
	scolor = r->color;
	for (b = 0; b < r->bevel; b++) {
		r->color = r->b_bright;
		q[0] = x0 - b - 1; q[1] = y1 + b + 1;
		q[2] = x0 - b - 1; q[3] = y0 - b - 1;
		q[4] = x1 + b + 1; q[5] = y0 - b - 1;
		polyline(r, q, 3);
		r->color = r->b_dark;
		q[0] = x1 + b + 1; q[1] = y0 - b - 1;
		q[2] = x1 + b + 1; q[3] = y1 + b + 1;
		q[4] = x0 - b - 1; q[5] = y1 + b + 1;
		polyline(r, q, 3);
	}
	r->color = r->b_fore;
	sfont = r->font;
	ssize = r->font_size;
	svert = r->vertical;
	r->font = 0;
	r->font_size = 1;
	r->vertical = 0;
	r->x = (x0 + x1) / 2 - plen[1] * 4;
	r->y = (y0 + y1) / 2 - 4;
	if (part[1])
		text(r, part[1], plen[1]);
	r->font = sfont;
	r->font_size = ssize;
	r->vertical = svert;
	r->color = scolor;
	if (plen[2] > 0)
		add_region(r, x0, y0, x1, y1, part[2], plen[2]);
}

/* ------------------------------------------------------------------ */
/* the commands */

static void level0(struct rip *r, uint32_t c, struct args *a)
{
	dmc_vt *t = r->t;
	struct raster *g;
	const uint32_t *s;
	int n, x0, y0, x1, y1, i, v;
	switch (c) {
	case 'w':
		x0 = arg(a); y0 = arg(a); x1 = arg(a); y1 = arg(a);
		text_window(r, x0, y0, x1, y1);
		break;
	case 'v':
		for (i = 0; i < 4; i++)
			r->viewport[i] = arg(a);
		r->has_viewport = 1;
		break;
	case '*':
		reset_windows(r);
		break;
	case 'e':
		vt_erase_display(t, 2, 0);
		vt_move_cursor(t, 0, t->top, 0);
		break;
	case 'E':
		if (!(g = R(r)))
			break;
		if (r->has_viewport)
			raster_clear_rect(g, r->viewport[0], r->viewport[1],
					  r->viewport[2], r->viewport[3], 0);
		else
			raster_clear_rect(g, 0, 0, WIDTH - 1, HEIGHT - 1, 0);
		break;
	case 'g':
		x0 = arg(a);
		y0 = arg(a);
		vt_move_cursor(t, x0, t->top + y0, 0);
		break;
	case 'H': vt_move_cursor(t, 0, t->top, 0); break;
	case '>': vt_erase_line(t, 0, 0); break;
	case 'c': r->color = arg(a) & 15; break;
	case 'Q':
		for (i = 0; i < 16; i++)
			r->palette[i] = ega(arg(a));
		break;
	case 'a':
		i = arg(a);
		v = arg(a);
		if (i >= 0 && i <= 15)
			r->palette[i] = ega(v);
		break;
	case 'W': r->xor_ = arg(a) == 1; break;
	case 'm': r->x = arg(a); r->y = arg(a); break;
	case 'T':
		s = rest(a, &n);
		text(r, s, n);
		break;
	case '@':
		r->x = arg(a);
		r->y = arg(a);
		s = rest(a, &n);
		text(r, s, n);
		break;
	case 'Y':
		r->font = arg(a);
		r->vertical = arg(a) == 1;
		r->font_size = clampi(arg(a), 1, 10);
		break;
	case 'X':
		x0 = arg(a);
		y0 = arg(a);
		set_up(r);
		if ((g = R(r)))
			raster_point(g, x0, y0);
		break;
	case 'L':
		x0 = arg(a); y0 = arg(a); x1 = arg(a); y1 = arg(a);
		line(r, x0, y0, x1, y1);
		break;
	case 'R': {
		int q[10];
		x0 = arg(a); y0 = arg(a); x1 = arg(a); y1 = arg(a);
		q[0] = x0; q[1] = y0; q[2] = x1; q[3] = y0;
		q[4] = x1; q[5] = y1; q[6] = x0; q[7] = y1;
		q[8] = x0; q[9] = y0;
		polyline(r, q, 5);
		break;
	}
	case 'B':
		x0 = arg(a); y0 = arg(a); x1 = arg(a); y1 = arg(a);
		box_fill(r, x0, y0, x1, y1);
		break;
	case 'C':
		x0 = arg(a); y0 = arg(a); v = arg(a);
		set_up(r);
		if ((g = R(r)))
			raster_ellipse(g, x0, y0, v, aspect(v), 0, 360);
		break;
	case 'A': {
		int cx = arg(a), cy = arg(a), st = arg(a), e = arg(a), rad = arg(a);
		arc(r, cx, cy, st, e, rad, aspect(rad));
		break;
	}
	case 'O': case 'V': {
		int cx = arg(a), cy = arg(a), st = arg(a), e = arg(a), rx = arg(a);
		arc(r, cx, cy, st, e, rx, arg(a));
		break;
	}
	case 'o': {
		int cx = arg(a), cy = arg(a), rx = arg(a), ry = arg(a);
		int xy[2 * MAX_ARC];
		n = raster_ellipse_points(cx, cy, rx, ry, 0, 360, xy, MAX_ARC);
		fill(r, xy, n);
		set_up(r);
		if ((g = R(r)))
			raster_ellipse(g, cx, cy, rx, ry, 0, 360);
		break;
	}
	case 'I': {
		int cx = arg(a), cy = arg(a), st = arg(a), e = arg(a), rad = arg(a);
		pie(r, cx, cy, st, e, rad, aspect(rad));
		break;
	}
	case 'i': {
		int cx = arg(a), cy = arg(a), st = arg(a), e = arg(a), rx = arg(a);
		pie(r, cx, cy, st, e, rx, arg(a));
		break;
	}
	case 'Z': {
		int p[8];
		for (i = 0; i < 8; i++)
			p[i] = arg(a);
		bezier(r, p, clampi(arg(a), 1, 256));
		break;
	}
	case 'P': case 'p': case 'l': {
		int *xy;
		n = arg(a);
		if (n < 2)
			break;
		xy = (int *)malloc(sizeof(int) * 2 * (size_t)(n + 1));
		if (!xy)
			break;
		for (i = 0; i < 2 * n; i++)
			xy[i] = arg(a);
		if (c == 'p')
			fill(r, xy, n);
		if (c != 'l') {
			xy[2 * n] = xy[0];
			xy[2 * n + 1] = xy[1];
			polyline(r, xy, n + 1);
		} else {
			polyline(r, xy, n);
		}
		free(xy);
		break;
	}
	case 'F':
		x0 = arg(a); y0 = arg(a); v = arg(a);
		set_up_fill(r);
		if ((g = R(r)))
			raster_flood(g, x0, y0, argb(r, v));
		break;
	case '=': {
		int style = arg(a), user = arg_w(a, 4);
		r->thick = arg(a) >= 3 ? 3 : 1;
		switch (style) {
		case 1: r->line_pattern = 0xcc; break;
		case 2: r->line_pattern = 0xe4; break;
		case 3: r->line_pattern = 0xf0; break;
		case 4: r->line_pattern = (user >> 8) & 0xff; break;
		default: r->line_pattern = 0xff;
		}
		break;
	}
	case 'S':
		r->fill_pattern = arg(a);
		r->fill_color = arg(a) & 15;
		break;
	case 's':
		for (i = 0; i < 8; i++)
			r->user_fill[i] = (uint8_t)(arg(a) & 0xff);
		r->fill_color = arg(a) & 15;
		r->fill_pattern = 12;
		break;
	}
}

static void level1(struct rip *r, uint32_t c, struct args *a)
{
	struct raster *g;
	const uint32_t *s;
	int n, x0, y0, x1, y1;
	switch (c) {
	case 'M':
		arg(a);
		x0 = arg(a); y0 = arg(a); x1 = arg(a); y1 = arg(a);
		arg(a); arg_w(a, 1); arg_w(a, 5);
		s = rest(a, &n);
		add_region(r, x0, y0, x1, y1, s, n);
		break;
	case 'K':
		clear_regions(r);
		break;
	case 'B': {
		int bevel, fore, bright, dark, surface;
		arg(a); arg(a); arg(a); arg_w(a, 4);
		bevel = arg(a);
		fore = arg(a);
		arg(a);
		bright = arg(a);
		dark = arg(a);
		surface = arg(a);
		r->bevel = bevel;
		r->b_fore = fore & 15;
		r->b_surface = surface & 15;
		r->b_bright = bright & 15;
		r->b_dark = dark & 15;
		break;
	}
	case 'U':
		x0 = arg(a); y0 = arg(a); x1 = arg(a); y1 = arg(a);
		arg(a); arg_w(a, 1); arg_w(a, 1);
		s = rest(a, &n);
		button(r, x0, y0, x1, y1, s, n);
		break;
	case 'T':
		r->block[0] = arg(a);
		r->block[1] = arg(a);
		r->block[2] = arg(a);
		r->block[3] = arg(a);
		r->block[4] = r->block[1];
		r->has_block = 1;
		break;
	case 't':
		if (!r->has_block)
			break;
		arg_w(a, 1);
		r->x = r->block[0];
		r->y = r->block[4];
		s = rest(a, &n);
		text(r, s, n);
		r->block[4] += cell_height(r);
		break;
	case 'E':
		r->has_block = 0;
		break;
	case 'C':
		x0 = arg(a); y0 = arg(a); x1 = arg(a); y1 = arg(a);
		if (!(g = R(r)))
			break;
		drop_clipboard(r);
		raster_grab(g, x0, y0, x1, y1, &r->clipboard);
		r->has_clipboard = 1;
		break;
	case 'P': {
		int px = arg(a), py = arg(a), mode = arg(a);
		if (r->has_clipboard && (g = R(r)))
			raster_put(g, &r->clipboard, px, py, mode == 1);
		break;
	}
	case 'G': {
		struct raster_grab gr;
		int dest;
		x0 = arg(a); y0 = arg(a); x1 = arg(a); y1 = arg(a);
		arg(a);
		dest = arg(a);
		if (!(g = R(r)))
			break;
		raster_grab(g, x0, y0, x1, y1, &gr);
		raster_put(g, &gr, x0, dest, 0);
		free(gr.px);
		break;
	}
	case 'F':
		/* a file query: none of the host's icons are here */
		vt_reply(r->t, "0\r\n");
		break;
	}
}

/* one command: its level (a digit) and letter, then its MegaNums and text */
static void run(struct rip *r, const uint32_t *cmd, int n)
{
	struct args a;
	int level = 0, at;
	if (n == 0)
		return;
	if (cmd[0] >= '1' && cmd[0] <= '9')
		level = (int)(cmd[0] - '0');
	at = level > 0 ? 1 : 0;
	if (at >= n)
		return;
	a.s = cmd;
	a.n = n;
	a.i = at + 1;
	if (level == 1)
		level1(r, cmd[at], &a);
	else if (level == 0)
		level0(r, cmd[at], &a);
}

/* a RIP line's commands, split at the |s that aren't escaped, each run */
static void run_line(struct rip *r, const uint32_t *s, int n)
{
	struct cps cmd = { NULL, 0, 0 };
	int i = 0;
	while (i < n) {
		uint32_t c = s[i];
		if (c == '\\' && i + 1 < n) {
			cps_add(&cmd, s[i + 1]);
			i += 2;
			continue;
		}
		if (c == '|') {
			run(r, cmd.v, cmd.n);
			cmd.n = 0;
		} else {
			cps_add(&cmd, c);
		}
		i++;
	}
	run(r, cmd.v, cmd.n);
	free(cmd.v);
}

/* ------------------------------------------------------------------ */
/* reading */

/* code points given to ANSI-BBS's parser after all */
static void give_back(struct rip *r, const int32_t *s, int n)
{
	int i;
	r->replaying = 1;
	for (i = 0; i < n; i++)
		r->replay(r->ctx, s[i]);
	r->replaying = 0;
	r->line_start = 0;
}

/* a RIP line's character: kept (a backslash's escape or continuation
   honoured) until the line ends, then run */
static void gather(struct rip *r, int32_t cp)
{
	if (r->escaped) {
		r->escaped = 0;
		if (cp == 0x0d) {
			r->swallow_lf = 1;
		} else if (cp != 0x0a) {
			cps_add(&r->line, '\\');
			cps_add(&r->line, (uint32_t)cp);
		}
		return;
	}
	switch (cp) {
	case '\\':
		r->escaped = 1;
		break;
	case 0x0d: case 0x0a: {
		struct cps l = r->line;
		r->in_line = 0;
		r->line_start = 1;
		r->swallow_lf = cp == 0x0d;
		memset(&r->line, 0, sizeof(r->line));
		run_line(r, l.v, l.n);
		free(l.v);
		break;
	}
	default:
		if (r->line.n < MAX_LINE)
			cps_add(&r->line, (uint32_t)cp);
	}
}

static int digit02(int32_t c)
{
	return c >= '0' && c <= '2';
}

/* ESC [ !, ESC [ 0 !, 1 !, 2 !: held until it is one (answered: RIP's
   version; off; on), else given back */
static int query(struct rip *r, int32_t cp)
{
	int n, i, full;
	int32_t h[8];
	if (r->held_n < 8)
		r->held[r->held_n++] = cp;
	n = r->held_n;
	full = n >= 3 && r->held[n - 1] == '!' && r->held[0] == 0x1b &&
		r->held[1] == '[';
	for (i = 2; full && i < n - 1; i++)
		if (!digit02(r->held[i]))
			full = 0;
	if (full) {
		int arg = n == 4 ? r->held[2] : '0';
		r->held_n = 0;
		if (arg == '0')
			rip_query(r);
		else if (arg == '1')
			r->enabled = 0;
		else
			r->enabled = 1;
		return 1;
	}
	if ((n == 1 && r->held[0] == 0x1b) ||
	    (n == 2 && r->held[1] == '[') ||
	    (n == 3 && digit02(r->held[2])))
		return 1;
	memcpy(h, r->held, sizeof(int32_t) * (size_t)n);
	r->held_n = 0;
	give_back(r, h, n);
	return 1;
}

int rip_intercept(struct rip *r, int32_t cp)
{
	if (r->replaying)
		return 0;
	if (r->swallow_lf) {
		r->swallow_lf = 0;
		if (cp == 0x0a)
			return 1;
	}
	if (r->in_line) {
		gather(r, cp);
		return 1;
	}
	if (r->held_n > 0 || cp == 0x1b)
		return query(r, cp);
	if (r->bang) {
		static const int32_t bang = '!';
		r->bang = 0;
		if (cp == '|') {
			r->in_line = 1;
			r->line.n = 0;
			return 1;
		}
		give_back(r, &bang, 1);
	}
	if (r->enabled && r->line_start && (cp == '!' || cp == 0x01 || cp == 0x02)) {
		r->bang = 1;
		return 1;
	}
	r->line_start = cp == 0x0d || cp == 0x0a;
	return 0;
}

void rip_query(struct rip *r)
{
	vt_reply(r->t, "RIPSCRIP015400");
}

/* a tap on a region or button (the last defined first): its host text,
   ^ and a letter a control */
int rip_tap(struct rip *r, double fx, double fy, dmc_buf *out)
{
	int px = (int)(fx * WIDTH), py = (int)(fy * HEIGHT), k, i;
	for (k = r->nregions - 1; k >= 0; k--) {
		const struct region *g = &r->regions[k];
		if (px < g->x0 || px > g->x1 || py < g->y0 || py > g->y1)
			continue;
		for (i = 0; i < g->n; i++) {
			uint32_t c = g->send[i];
			if (c == '^' && i + 1 < g->n) {
				uint32_t u = g->send[i + 1];
				if (u >= 'a' && u <= 'z')
					u -= 0x20;
				buf_addc(out, (char)(u & 0x1f));
				i++;
				continue;
			}
			buf_addc(out, (char)cp437_encode(c));
		}
		return 1;
	}
	return 0;
}

void rip_flush(struct rip *r)
{
	layer_flush(&r->layer);
}

/* RIS: the picture, windows and regions as at the start, RIP on, the
   80x43 asked for again */
void rip_reset(struct rip *r)
{
	r->in_line = 0;
	r->line.n = 0;
	r->bang = 0;
	r->escaped = 0;
	r->swallow_lf = 0;
	r->held_n = 0;
	r->enabled = 1;
	r->line_start = 1;
	reset_windows(r);
	r->t->requested_cols = 80;
	r->t->requested_rows = 43;
}

struct rip *rip_new(dmc_vt *t, void (*replay)(void *, int32_t), void *ctx)
{
	struct rip *r = (struct rip *)calloc(1, sizeof(*r));
	int i;
	if (!r)
		return NULL;
	r->t = t;
	r->replay = replay;
	r->ctx = ctx;
	layer_init(&r->layer, t, WIDTH, HEIGHT, NULL, NULL);
	r->line_start = 1;
	r->enabled = 1;
	r->color = 15;
	r->line_pattern = 0xff;
	r->thick = 1;
	r->fill_pattern = 1;
	memset(r->user_fill, 0xff, sizeof(r->user_fill));
	r->fill_color = 15;
	r->font_size = 1;
	for (i = 0; i < 16; i++)
		r->palette[i] = ega(ega_start[i]);
	r->bevel = 2;
	r->b_fore = 0;
	r->b_surface = 7;
	r->b_bright = 15;
	r->b_dark = 8;
	t->requested_cols = 80;
	t->requested_rows = 43;
	return r;
}

void rip_free(struct rip *r)
{
	if (!r)
		return;
	clear_regions(r);
	free(r->regions);
	drop_clipboard(r);
	free(r->line.v);
	layer_free(&r->layer);
	free(r);
}
