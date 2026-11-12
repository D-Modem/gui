/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * vt_gfx.c - the terminal's graphics: sixel images (the app's Sixel), a
 * graphics terminal's picture over the screen (Raster, PictureLayer) and
 * the Tektronix 4010/4014 (TektronixInterpreter).
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "vtint.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static int maxi(int a, int b)
{
	return a > b ? a : b;
}

static int mini(int a, int b)
{
	return a < b ? a : b;
}

static int roundi(double v)
{
	return (int)floor(v + 0.5);
}

/* ------------------------------------------------------------------ */
/* sixel */

struct sixel {
	int transparent;
	uint32_t own[256];
	uint32_t *palette;
	int max_w, max_h;
	int color, x, y, repeat;
	uint32_t *buf;
	int buf_w, buf_h;
	int width, height;
	int command, params[5], nparams;
	int failed;
};

static int pct(int v)
{
	return (clampi(v, 0, 100) * 255 + 50) / 100;
}

static uint32_t rgb(int r, int g, int b)
{
	return 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static int hue(double p, double q, double t)
{
	double v;
	if (t < 0)
		t += 1.0;
	if (t > 1)
		t -= 1.0;
	if (t < 1.0 / 6)
		v = p + (q - p) * 6 * t;
	else if (t < 1.0 / 2)
		v = q;
	else if (t < 2.0 / 3)
		v = p + (q - p) * (2.0 / 3 - t) * 6;
	else
		v = p;
	return clampi(roundi(v * 255), 0, 255);
}

uint32_t sixel_hls(int hdeg, int lpct, int spct)
{
	double l = clampi(lpct, 0, 100) / 100.0, s = clampi(spct, 0, 100) / 100.0;
	/* DEC's hue is the usual one turned by 240 degrees (its 0 is blue) */
	double h = (((hdeg + 240) % 360) + 360) % 360 / 360.0, q, p;
	if (s == 0.0) {
		int v = roundi(l * 255);
		return rgb(v, v, v);
	}
	q = l < 0.5 ? l * (1 + s) : l + s - l * s;
	p = 2 * l - q;
	return rgb(hue(p, q, h + 1.0 / 3), hue(p, q, h), hue(p, q, h - 1.0 / 3));
}

void sixel_default_palette(uint32_t p[256])
{
	static const uint8_t vt340[48] = {
		0, 0, 0, 20, 20, 80, 80, 13, 13, 20, 80, 20, 80, 20, 80, 20, 80, 80,
		80, 80, 20, 53, 53, 53, 26, 26, 26, 33, 33, 60, 60, 26, 26, 33, 60, 33,
		60, 33, 60, 33, 60, 60, 60, 60, 33, 80, 80, 80
	};
	static const int steps[6] = { 0, 95, 135, 175, 215, 255 };
	int i;
	for (i = 0; i < 16; i++)
		p[i] = rgb(pct(vt340[i * 3]), pct(vt340[i * 3 + 1]), pct(vt340[i * 3 + 2]));
	for (i = 16; i < 232; i++) {
		int n = i - 16;
		p[i] = rgb(steps[n / 36], steps[(n / 6) % 6], steps[n % 6]);
	}
	for (i = 232; i < 256; i++) {
		int v = 8 + (i - 232) * 10;
		p[i] = rgb(v, v, v);
	}
}

struct sixel *sixel_new(int transparent, uint32_t *shared, int max_w, int max_h)
{
	struct sixel *s = (struct sixel *)calloc(1, sizeof(*s));
	if (!s)
		return NULL;
	s->transparent = transparent;
	if (shared) {
		s->palette = shared;
	} else {
		sixel_default_palette(s->own);
		s->palette = s->own;
	}
	s->max_w = max_w > 0 ? max_w : 1;
	s->max_h = max_h > 0 ? max_h : 1;
	s->repeat = 1;
	return s;
}

void sixel_free(struct sixel *s)
{
	if (s) {
		free(s->buf);
		free(s);
	}
}

/* the buffer made at least w x h (doubling, so that growing is cheap) */
static int grow(struct sixel *s, int w, int h)
{
	int nw, nh, r;
	uint32_t *nb;
	if (w <= s->buf_w && h <= s->buf_h)
		return 1;
	if (s->failed)
		return 0;
	nw = maxi(s->buf_w, mini(s->max_w, maxi(maxi(w, s->buf_w * 2), 64)));
	nh = maxi(s->buf_h, mini(s->max_h + 6, maxi(maxi(h, s->buf_h * 2), 64)));
	nb = (uint32_t *)calloc((size_t)nw * nh, sizeof(uint32_t));
	if (!nb) {
		s->failed = 1;
		return 0;
	}
	for (r = 0; r < s->buf_h; r++)
		memcpy(nb + (size_t)r * nw, s->buf + (size_t)r * s->buf_w,
		       (size_t)s->buf_w * sizeof(uint32_t));
	free(s->buf);
	s->buf = nb;
	s->buf_w = nw;
	s->buf_h = nh;
	return 1;
}

static void end_command(struct sixel *s)
{
	int n;
	switch (s->command) {
	case '!':
		s->repeat = s->params[0] > 1 ? s->params[0] : 1;
		break;
	case '#':
		n = s->params[0] & 0xff;
		if (s->nparams >= 5)
			s->palette[n] = s->params[1] == 1 ?
				sixel_hls(s->params[2], s->params[3], s->params[4]) :
				rgb(pct(s->params[2]), pct(s->params[3]), pct(s->params[4]));
		s->color = n;
		break;
	case '"':
		if (s->nparams >= 4 && s->params[2] > 0 && s->params[3] > 0) {
			int w = mini(s->params[2], s->max_w), h = mini(s->params[3], s->max_h);
			if (grow(s, w, h)) {
				s->width = maxi(s->width, w);
				s->height = maxi(s->height, h);
			}
		}
		break;
	}
	s->command = 0;
}

static void sixel_bits(struct sixel *s, int bits)
{
	int n = s->repeat, b, right, px;
	s->repeat = 1;
	if (bits && s->y < s->max_h) {
		right = mini(s->x + n, s->max_w);
		if (right > s->x && grow(s, right, s->y + 6)) {
			uint32_t c = s->palette[s->color];
			for (b = 0; b < 6; b++) {
				int row = s->y + b;
				uint32_t *base;
				if (!(bits & (1 << b)))
					continue;
				if (row >= s->max_h)
					break;
				base = s->buf + (size_t)row * s->buf_w;
				for (px = s->x; px < right; px++)
					base[px] = c;
				if (row + 1 > s->height)
					s->height = row + 1;
			}
			if (right > s->width)
				s->width = right;
		}
	}
	if (s->x < 1 << 20)
		s->x += n;
}

void sixel_feed(struct sixel *s, int32_t cp)
{
	if (s->command) {
		if (cp >= '0' && cp <= '9') {
			if (s->nparams == 0)
				s->nparams = 1;
			if (s->nparams <= 5) {
				int *v = &s->params[s->nparams - 1];
				*v = mini(*v * 10 + (cp - '0'), 99999);
			}
			return;
		}
		if (cp == ';') {
			if (s->nparams == 0)
				s->nparams = 1;
			if (s->nparams < 5) {
				s->params[s->nparams] = 0;
				s->nparams++;
			}
			return;
		}
		end_command(s);
	}
	if (cp >= 0x3f && cp <= 0x7e) {
		sixel_bits(s, cp - 0x3f);
	} else if (cp == '#' || cp == '!' || cp == '"') {
		s->command = cp;
		s->nparams = 0;
		memset(s->params, 0, sizeof(s->params));
	} else if (cp == '$') {
		s->x = 0;
	} else if (cp == '-') {
		s->x = 0;
		if (s->y < 1 << 20)
			s->y += 6;
	}
}

struct vt_image *sixel_finish(dmc_vt *t, struct sixel *s, int cw, int ch)
{
	int h, r, i;
	struct vt_image *img;
	(void)t;
	if (s->command)
		end_command(s);
	h = mini(s->height, s->max_h);
	if (s->width <= 0 || h <= 0 || !s->buf)
		return NULL;
	img = image_new(s->width, h, cw, ch, 0);
	for (r = 0; r < h; r++)
		memcpy(img->pixels + (size_t)r * s->width, s->buf + (size_t)r * s->buf_w,
		       (size_t)s->width * sizeof(uint32_t));
	if (!s->transparent) {
		/* the background: register 0 as the image left it */
		uint32_t bg = s->palette[0];
		for (i = 0; i < s->width * h; i++)
			if (!img->pixels[i])
				img->pixels[i] = bg;
	}
	return img;
}

/* ------------------------------------------------------------------ */
/* the raster */

struct raster *raster_new(int w, int h, int lw, int lh)
{
	struct raster *r = (struct raster *)calloc(1, sizeof(*r));
	if (!r)
		return NULL;
	r->width = w > 0 ? w : 1;
	r->height = h > 0 ? h : 1;
	r->lw = lw > 0 ? lw : 1;
	r->lh = lh > 0 ? lh : 1;
	r->px = (uint32_t *)calloc((size_t)r->width * r->height, sizeof(uint32_t));
	if (!r->px) {
		free(r);
		return NULL;
	}
	r->mode = R_OVERLAY;
	r->color = 0xffffffffu;
	r->pattern = 0xff;
	return r;
}

void raster_free(struct raster *r)
{
	if (r) {
		free(r->px);
		free(r);
	}
}

int raster_px(const struct raster *r, int x)
{
	return (int)((long long)x * r->width / r->lw);
}

int raster_py(const struct raster *r, int y)
{
	return (int)((long long)y * r->height / r->lh);
}

void raster_clear(struct raster *r, uint32_t c)
{
	int i, n = r->width * r->height;
	for (i = 0; i < n; i++)
		r->px[i] = c;
	r->dirty = 1;
}

static void put(struct raster *r, int x, int y, int on)
{
	uint32_t *p;
	if (x < 0 || x >= r->width || y < 0 || y >= r->height)
		return;
	if (r->has_clip && (x < r->clip[0] || y < r->clip[1] || x > r->clip[2] ||
			    y > r->clip[3]))
		return;
	p = &r->px[y * r->width + x];
	switch (r->mode) {
	case R_OVERLAY: if (on) *p = r->color; break;
	case R_REPLACE: *p = on ? r->color : r->background; break;
	case R_COMPLEMENT: if (on) *p = (*p ^ 0x00ffffffu) | 0xff000000u; break;
	case R_ERASE: *p = r->background; break;
	}
	r->dirty = 1;
}

void raster_point(struct raster *r, int x, int y)
{
	put(r, raster_px(r, x), raster_py(r, y), 1);
}

static void pixel_line(struct raster *r, int ax, int ay, int bx, int by)
{
	int x = ax, y = ay, dx = abs(bx - ax), dy = -abs(by - ay);
	int sx = ax < bx ? 1 : -1, sy = ay < by ? 1 : -1, err = dx + dy, e2;
	for (;;) {
		put(r, x, y, (r->pattern & (0x80 >> (r->pattern_step & 7))) != 0);
		r->pattern_step++;
		if (x == bx && y == by)
			break;
		e2 = 2 * err;
		if (e2 >= dy) {
			err += dy;
			x += sx;
		}
		if (e2 <= dx) {
			err += dx;
			y += sy;
		}
	}
}

void raster_line(struct raster *r, int x0, int y0, int x1, int y1)
{
	pixel_line(r, raster_px(r, x0), raster_py(r, y0), raster_px(r, x1),
		   raster_py(r, y1));
}

static void pixel_arc(struct raster *r, int cx, int cy, double rx, double ry,
		      double start, double sweep)
{
	double big = rx > ry ? rx : ry;
	int steps = (int)(fabs(sweep) / 360 * (big * 2 > 8.0 ? big * 2 : 8.0) * M_PI);
	int lx = 0, ly = 0, i;
	if (steps < 16)
		steps = 16;
	if (steps > 100000)
		steps = 100000;
	for (i = 0; i <= steps; i++) {
		double a = (start + sweep * i / steps) * M_PI / 180;
		int x = cx + roundi(rx * cos(a)), y = cy - roundi(ry * sin(a));
		if (i > 0)
			pixel_line(r, lx, ly, x, y);
		lx = x;
		ly = y;
	}
}

void raster_arc(struct raster *r, int cx, int cy, int rad, double start,
		double sweep)
{
	double pr = raster_px(r, rad);
	pixel_arc(r, raster_px(r, cx), raster_py(r, cy), pr, pr, start, sweep);
}

void raster_ellipse(struct raster *r, int cx, int cy, int rx, int ry,
		    double start, double sweep)
{
	pixel_arc(r, raster_px(r, cx), raster_py(r, cy),
		  (double)rx * r->width / r->lw, (double)ry * r->height / r->lh,
		  start, sweep);
}

int raster_ellipse_points(int cx, int cy, int rx, int ry, double start,
			  double sweep, int *xy, int max)
{
	int n = (int)(fabs(sweep) / 360 * 64), i, k = 0;
	if (n < 16)
		n = 16;
	for (i = 0; i <= n && k < max; i++) {
		double a = (start + sweep * i / n) * M_PI / 180;
		xy[2 * k] = cx + roundi(rx * cos(a));
		xy[2 * k + 1] = cy - roundi(ry * sin(a));
		k++;
	}
	return k;
}

static void fill_pixel(struct raster *r, int x, int y)
{
	const uint8_t *p = r->fill_pattern;
	if (!p) {
		put(r, x, y, 1);
		return;
	}
	if ((p[y & 7] >> (7 - (x & 7))) & 1) {
		put(r, x, y, 1);
	} else if (r->fill_background) {
		uint32_t c = r->color;
		r->color = r->fill_background;
		put(r, x, y, 1);
		r->color = c;
	}
}

static int cmp_int(const void *a, const void *b)
{
	int x = *(const int *)a, y = *(const int *)b;
	return x < y ? -1 : x > y;
}

void raster_fill(struct raster *r, const int *xy, int n)
{
	int *p, *xs, top, bottom, i, y, k, x;
	if (n < 3)
		return;
	p = (int *)malloc(sizeof(int) * 2 * (size_t)n);
	xs = (int *)malloc(sizeof(int) * (size_t)n);
	if (!p || !xs) {
		free(p);
		free(xs);
		return;
	}
	top = 1 << 30;
	bottom = -(1 << 30);
	for (i = 0; i < n; i++) {
		p[2 * i] = raster_px(r, xy[2 * i]);
		p[2 * i + 1] = raster_py(r, xy[2 * i + 1]);
		top = mini(top, p[2 * i + 1]);
		bottom = maxi(bottom, p[2 * i + 1]);
	}
	top = maxi(top, 0);
	bottom = mini(bottom, r->height - 1);
	for (y = top; y <= bottom; y++) {
		int nx = 0;
		for (i = 0; i < n; i++) {
			int x0 = p[2 * i], y0 = p[2 * i + 1];
			int x1 = p[2 * ((i + 1) % n)], y1 = p[2 * ((i + 1) % n) + 1];
			if (y0 == y1)
				continue;
			if (y >= mini(y0, y1) && y < maxi(y0, y1))
				xs[nx++] = x0 + (int)((long long)(y - y0) * (x1 - x0) / (y1 - y0));
		}
		qsort(xs, nx, sizeof(int), cmp_int);
		for (k = 0; k + 1 < nx; k += 2) {
			int a = maxi(xs[k], -1), b = mini(xs[k + 1], r->width);
			for (x = a; x <= b; x++)
				fill_pixel(r, x, y);
		}
	}
	free(p);
	free(xs);
}

void raster_flood(struct raster *r, int x, int y, uint32_t border)
{
	int sx = raster_px(r, x), sy = raster_py(r, y), w = r->width, h = r->height;
	uint8_t *seen;
	int *stack, n = 0, cap;
	if (sx < 0 || sx >= w || sy < 0 || sy >= h || r->px[sy * w + sx] == border)
		return;
	seen = (uint8_t *)calloc((size_t)w * h, 1);
	cap = 4096;
	stack = (int *)malloc(sizeof(int) * (size_t)cap);
	if (!seen || !stack) {
		free(seen);
		free(stack);
		return;
	}
#define INSIDE(i) (!seen[i] && r->px[i] != border && \
	(!r->has_clip || ((i) % w >= r->clip[0] && (i) / w >= r->clip[1] && \
			  (i) % w <= r->clip[2] && (i) / w <= r->clip[3])))
	stack[n++] = sy * w + sx;
	while (n > 0) {
		int i = stack[--n], row, l, rr, xx, k;
		if (!INSIDE(i))
			continue;
		row = i / w;
		l = rr = i % w;
		while (l > 0 && INSIDE(row * w + l - 1))
			l--;
		while (rr < w - 1 && INSIDE(row * w + rr + 1))
			rr++;
		for (xx = l; xx <= rr; xx++)
			seen[row * w + xx] = 1;
		for (xx = l; xx <= rr; xx++)
			fill_pixel(r, xx, row);
		for (k = -1; k <= 1; k += 2) {
			int ny = row + k;
			if (ny < 0 || ny >= h)
				continue;
			for (xx = l; xx <= rr; xx++)
				if (INSIDE(ny * w + xx)) {
					if (n == cap) {
						int *ns;
						cap *= 2;
						ns = (int *)realloc(stack, sizeof(int) * (size_t)cap);
						if (!ns) {
							free(seen);
							free(stack);
							return;
						}
						stack = ns;
					}
					stack[n++] = ny * w + xx;
				}
		}
	}
#undef INSIDE
	free(seen);
	free(stack);
	r->dirty = 1;
}

void raster_grab(const struct raster *r, int x0, int y0, int x1, int y1,
		 struct raster_grab *g)
{
	int l = clampi(raster_px(r, mini(x0, x1)), 0, r->width - 1);
	int t = clampi(raster_py(r, mini(y0, y1)), 0, r->height - 1);
	int rr = clampi(raster_px(r, maxi(x0, x1) + 1), l + 1, r->width);
	int b = clampi(raster_py(r, maxi(y0, y1) + 1), t + 1, r->height);
	int yy;
	g->width = rr - l;
	g->height = b - t;
	g->px = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)g->width * g->height);
	if (!g->px) {
		g->width = g->height = 0;
		return;
	}
	for (yy = 0; yy < g->height; yy++)
		memcpy(g->px + (size_t)yy * g->width, r->px + (size_t)(t + yy) * r->width + l,
		       sizeof(uint32_t) * (size_t)g->width);
}

void raster_put(struct raster *r, const struct raster_grab *g, int x, int y,
		int xor_)
{
	int l = raster_px(r, x), t = raster_py(r, y), xx, yy;
	for (yy = 0; yy < g->height; yy++)
		for (xx = 0; xx < g->width; xx++) {
			int dx = l + xx, dy = t + yy;
			uint32_t v, *p;
			if (dx < 0 || dx >= r->width || dy < 0 || dy >= r->height)
				continue;
			if (r->has_clip && (dx < r->clip[0] || dy < r->clip[1] ||
					    dx > r->clip[2] || dy > r->clip[3]))
				continue;
			v = g->px[yy * g->width + xx];
			p = &r->px[dy * r->width + dx];
			*p = xor_ ? (*p ^ v) | ((*p | v) ? 0xff000000u : 0) : v;
		}
	r->dirty = 1;
}

void raster_pixel_rect(const struct raster *r, int x0, int y0, int x1, int y1,
		       int out[4])
{
	out[0] = raster_px(r, mini(x0, x1));
	out[1] = raster_py(r, mini(y0, y1));
	out[2] = raster_px(r, maxi(x0, x1) + 1) - 1;
	out[3] = raster_py(r, maxi(y0, y1) + 1) - 1;
}

void raster_clear_rect(struct raster *r, int x0, int y0, int x1, int y1,
		       uint32_t c)
{
	int q[4], xx, yy;
	raster_pixel_rect(r, x0, y0, x1, y1, q);
	for (yy = maxi(0, q[1]); yy <= mini(r->height - 1, q[3]); yy++)
		for (xx = maxi(0, q[0]); xx <= mini(r->width - 1, q[2]); xx++)
			r->px[yy * r->width + xx] = c;
	r->dirty = 1;
}

static void glyph(struct raster *r, uint32_t cp, int x, int y,
		  const struct vt_font *f, int cw, int ch)
{
	int left, top, w, h, xx, yy;
	if (font_index(f, cp) < 0)
		return;
	left = raster_px(r, x);
	top = raster_py(r, y);
	w = maxi(1, raster_px(r, x + cw) - left);
	h = maxi(1, raster_py(r, y + ch) - top);
	for (yy = 0; yy < h; yy++) {
		/* (shrunk: the rows it covers merged, so a thin stroke stays) */
		int r0 = yy * f->h / h, r1 = maxi(r0 + 1, (yy + 1) * f->h / h), k;
		uint32_t row = 0;
		for (k = r0; k < r1; k++)
			row |= font_row(f, cp, k);
		if (!row)
			continue;
		for (xx = 0; xx < w; xx++) {
			int c0 = xx * f->w / w, c1 = maxi(c0 + 1, (xx + 1) * f->w / w), on = 0;
			for (k = c0; k < c1; k++)
				if ((row >> (f->w - 1 - k)) & 1)
					on = 1;
			if (on)
				put(r, left + xx, top + yy, 1);
		}
	}
}

int raster_text(struct raster *r, int x, int y, const uint32_t *cps, int n,
		const struct vt_font *f, int cw, int ch)
{
	int i;
	if (!f)
		return x + n * cw;
	for (i = 0; i < n; i++) {
		glyph(r, cps[i], x, y, f, cw, ch);
		x += cw;
	}
	return x;
}

struct vt_image *raster_take(dmc_vt *t, struct raster *r, int cw, int ch)
{
	struct vt_image *img = image_new(r->width, r->height, cw, ch, 1);
	(void)t;
	memcpy(img->pixels, r->px, sizeof(uint32_t) * (size_t)r->width * r->height);
	r->dirty = 0;
	return img;
}

/* ------------------------------------------------------------------ */
/* the picture layer */

static int pixel_w(const dmc_vt *t)
{
	return maxi(1, t->cols * t->cell_pw);
}

static int pixel_h(const dmc_vt *t)
{
	return maxi(1, t->rows * t->cell_ph);
}

static struct raster *make(struct picture_layer *l)
{
	struct raster *r = raster_new(pixel_w(l->t), pixel_h(l->t), l->lw, l->lh);
	if (r && l->setup)
		l->setup(r, l->ctx);
	return r;
}

void layer_init(struct picture_layer *l, dmc_vt *t, int lw, int lh,
		void (*setup)(struct raster *, void *), void *ctx)
{
	l->t = t;
	l->lw = lw;
	l->lh = lh;
	l->setup = setup;
	l->ctx = ctx;
	l->r = make(l);
}

void layer_reset(struct picture_layer *l)
{
	struct raster *r = make(l);
	if (!r)
		return;
	raster_free(l->r);
	l->r = r;
	raster_clear(r, 0);
}

struct raster *layer_raster(struct picture_layer *l)
{
	if (!l->r || pixel_w(l->t) != l->r->width || pixel_h(l->t) != l->r->height)
		layer_reset(l);
	return l->r;
}

void layer_flush(struct picture_layer *l)
{
	struct raster *r = layer_raster(l);
	if (r && r->dirty)
		vt_show_picture(l->t, raster_take(l->t, r, l->t->cell_pw, l->t->cell_ph));
}

void layer_free(struct picture_layer *l)
{
	raster_free(l->r);
	l->r = NULL;
}

/* ------------------------------------------------------------------ */
/* the Tektronix 4010/4014 */

#define TEK_W 4096
#define TEK_H 3120
#define TEK_STEP 4
#define TEK_INK 0xffc8ffc8u

enum { TM_ALPHA, TM_GRAPH, TM_POINT, TM_INCREMENTAL };

static const int char_w[4] = { 56, 51, 34, 31 };
static const int char_h[4] = { 88, 82, 53, 48 };
static const int styles[8] = { 0xff, 0xaa, 0xe4, 0xcc, 0xf0, 0xff, 0xff, 0xff };

struct tek {
	struct vt_interp in;
	void (*on_exit)(void *);
	void *ctx;
	struct picture_layer layer;
	int mode, x, y, dark, pen, size, escaped;
	int hi_x, hi_y, lo_x, lo_y, extra, last_lo_y;
};

static void tek_setup(struct raster *r, void *ctx)
{
	(void)ctx;
	r->color = TEK_INK;
}

static void tek_home(struct tek *k)
{
	k->mode = TM_ALPHA;
	k->x = 0;
	k->y = TEK_H - char_h[k->size];
}

static int row(int ty)
{
	return TEK_H - 1 - ty;
}

static void tek_process(struct vt_interp *in, int32_t cp);

static void tek_report(struct tek *k)
{
	int sx = k->x >> 2, sy = k->y >> 2;
	char s[8];
	s[0] = (char)(k->mode == TM_ALPHA ? 0x34 : 0x30);
	s[1] = (char)(0x20 | ((sx >> 5) & 0x1f));
	s[2] = (char)(0x20 | (sx & 0x1f));
	s[3] = (char)(0x20 | ((sy >> 5) & 0x1f));
	s[4] = (char)(0x20 | (sy & 0x1f));
	s[5] = '\r';
	s[6] = 0;
	vt_reply(k->in.t, s);
}

static void tek_esc(struct tek *k, int c)
{
	if (c == 0x0c) {
		raster_clear(layer_raster(&k->layer), 0);
		tek_home(k);
	} else if (c == 0x05) {
		tek_report(k);
	} else if (c >= '8' && c <= ';') {
		k->size = c - '8';
	} else if (c >= 0x60 && c <= 0x67) {
		layer_raster(&k->layer)->pattern = styles[(c - 0x60) & 7];
	} else if (c == 0x03) {
		if (k->on_exit)
			k->on_exit(k->ctx);
	} else if (c < 0x20) {
		tek_process(&k->in, c);
	}
}

static void tek_alpha(struct tek *k, int c)
{
	int cw = char_w[k->size], ch = char_h[k->size];
	uint32_t cp = (uint32_t)c;
	switch (c) {
	case 0x08: k->x = maxi(k->x - cw, 0); break;
	case 0x09: k->x += cw; break;
	case 0x0a:
		k->y -= ch;
		if (k->y < 0)
			k->y = TEK_H - ch;
		break;
	case 0x0b: k->y = mini(k->y + ch, TEK_H - ch); break;
	default:
		if (c < 0x20 || c > 0x7e)
			break;
		if (k->x + cw > TEK_W) {
			k->x = 0;
			k->y -= ch;
			if (k->y < 0)
				k->y = TEK_H - ch;
		}
		if (c != 0x20)
			raster_text(layer_raster(&k->layer), k->x, row(k->y + ch), &cp, 1,
				    &vt_font_vga, cw, ch);
		k->x += cw;
	}
}

static void tek_to(struct tek *k, int nx, int ny)
{
	if (k->mode == TM_POINT)
		raster_point(layer_raster(&k->layer), nx, row(ny));
	else if (k->dark)
		k->dark = 0;
	else
		raster_line(layer_raster(&k->layer), k->x, row(k->y), nx, row(ny));
	k->x = nx;
	k->y = ny;
}

static void tek_address(struct tek *k, int c)
{
	int v = c & 0x1f, nx, ny;
	switch (c >> 5) {
	case 1:
		if (k->last_lo_y)
			k->hi_x = v;
		else
			k->hi_y = v;
		k->last_lo_y = 0;
		break;
	case 3:
		if (k->last_lo_y)
			k->extra = k->lo_y;
		k->lo_y = v;
		k->last_lo_y = 1;
		break;
	case 2:
		k->lo_x = v;
		k->last_lo_y = 0;
		nx = (k->hi_x << 7) | (k->lo_x << 2) | (k->extra & 3);
		ny = (k->hi_y << 7) | (k->lo_y << 2) | ((k->extra >> 2) & 3);
		k->extra = 0;
		tek_to(k, nx, ny);
		break;
	}
}

static void tek_incremental(struct tek *k, int c)
{
	int dx, dy, nx, ny;
	switch (c) {
	case ' ': k->pen = 0; return;
	case 'P': k->pen = 1; return;
	case 'D': dx = 0; dy = 1; break;
	case 'E': dx = 1; dy = 1; break;
	case 'A': dx = 1; dy = 0; break;
	case 'I': dx = 1; dy = -1; break;
	case 'H': dx = 0; dy = -1; break;
	case 'J': dx = -1; dy = -1; break;
	case 'B': dx = -1; dy = 0; break;
	case 'F': dx = -1; dy = 1; break;
	default: return;
	}
	nx = clampi(k->x + dx * TEK_STEP, 0, TEK_W - 1);
	ny = clampi(k->y + dy * TEK_STEP, 0, TEK_H - 1);
	if (k->pen)
		raster_line(layer_raster(&k->layer), k->x, row(k->y), nx, row(ny));
	k->x = nx;
	k->y = ny;
}

static void tek_process(struct vt_interp *in, int32_t cp)
{
	struct tek *k = (struct tek *)in;
	int c = cp & 0x7f;
	if (k->escaped) {
		k->escaped = 0;
		tek_esc(k, c);
		return;
	}
	switch (c) {
	case 0x1b: k->escaped = 1; break;
	case 0x1d: k->mode = TM_GRAPH; k->dark = 1; break;
	case 0x1c: k->mode = TM_POINT; break;
	case 0x1e: k->mode = TM_INCREMENTAL; k->pen = 0; break;
	case 0x1f: k->mode = TM_ALPHA; break;
	case 0x07: vt_bell(in->t); break;
	case 0x0d: k->mode = TM_ALPHA; k->x = 0; break;
	default:
		switch (k->mode) {
		case TM_ALPHA: tek_alpha(k, c); break;
		case TM_GRAPH: case TM_POINT: if (c >= 0x20) tek_address(k, c); break;
		case TM_INCREMENTAL: tek_incremental(k, c); break;
		}
	}
}

static void tek_flush(struct vt_interp *in)
{
	layer_flush(&((struct tek *)in)->layer);
}

static void tek_reset(struct vt_interp *in)
{
	struct tek *k = (struct tek *)in;
	layer_reset(&k->layer);
	k->size = 0;
	k->escaped = 0;
	tek_home(k);
}

static void tek_destroy(struct vt_interp *in)
{
	struct tek *k = (struct tek *)in;
	layer_free(&k->layer);
	free(k);
}

static const struct vt_interp_ops tek_ops = {
	tek_process, tek_reset, NULL, NULL, NULL, NULL, NULL, NULL, tek_flush,
	NULL, tek_destroy
};

struct vt_interp *tek_new(dmc_vt *t, void (*on_exit)(void *), void *ctx)
{
	struct tek *k = (struct tek *)calloc(1, sizeof(*k));
	if (!k)
		return NULL;
	k->in.ops = &tek_ops;
	k->in.t = t;
	k->on_exit = on_exit;
	k->ctx = ctx;
	layer_init(&k->layer, t, TEK_W, TEK_H, tek_setup, NULL);
	tek_home(k);
	return &k->in;
}
