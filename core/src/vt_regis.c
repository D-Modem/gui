/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * vt_regis.c - ReGIS, DEC's graphics language (a VT340's; the app's
 * Regis): what comes in DCS p ... ST, each command run once the next has
 * begun (or the string ended).  P (position), V (vectors), C (circles,
 * arcs and curves), F (fill), T (text), W (writing controls), S (screen
 * controls, the colour map among them), R (reports), L (loadable
 * alphabets) and @ (macrographs), drawn on an 800x480 screen over the
 * text in the 16 colour registers it shares with the sixels.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vtint.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define RW 800
#define RH 480
#define MAX_PENDING (1 << 20)
#define MAX_DEPTH 8
#define MAX_ROWS 32
#define STEPS 8
#define MAX_POS 4
#define MAX_CHARS 256

static const int patterns[10] = {
	0x00, 0xff, 0xf0, 0xe4, 0xaa, 0xea, 0x88, 0x84, 0xc8, 0xee
};
static const int dirs[8][2] = {
	{ 1, 0 }, { 1, -1 }, { 0, -1 }, { -1, -1 }, { -1, 0 }, { -1, 1 }, { 0, 1 },
	{ 1, 1 }
};

/* a run of code points */
struct cps {
	uint32_t *p;
	int n, cap;
};

static void cps_add(struct cps *c, uint32_t v)
{
	if (c->n == c->cap) {
		int cap = c->cap ? c->cap * 2 : 64;
		uint32_t *np = (uint32_t *)realloc(c->p, sizeof(uint32_t) * (size_t)cap);
		if (!np)
			return;
		c->p = np;
		c->cap = cap;
	}
	c->p[c->n++] = v;
}

static void cps_free(struct cps *c)
{
	free(c->p);
	c->p = NULL;
	c->n = c->cap = 0;
}

/* a list of points (screen coordinates) */
struct pts {
	int *xy;
	int n, cap;
};

static void pts_add(struct pts *p, int x, int y)
{
	if (p->n == p->cap) {
		int cap = p->cap ? p->cap * 2 : 64;
		int *nx;
		if (cap > 1 << 20)
			return;
		nx = (int *)realloc(p->xy, sizeof(int) * 2 * (size_t)cap);
		if (!nx)
			return;
		p->xy = nx;
		p->cap = cap;
	}
	p->xy[2 * p->n] = x;
	p->xy[2 * p->n + 1] = y;
	p->n++;
}

static void pts_free(struct pts *p)
{
	free(p->xy);
	p->xy = NULL;
	p->n = p->cap = 0;
}

/* a coordinate of a position: its value, and whether it's from the
   position (signed) */
struct part {
	int has, value, relative;
};

struct pos {
	struct part x, y;
	int empty;
};

struct opt {
	int letter;
	int has_num, num;
	char digits[16];
	struct pos positions[MAX_POS];
	int npos;
	struct opt *sub;
	int nsub;
	struct cps str;
	int has_str;
};

struct writing {
	int mode, color, pattern, negate, multiplier, shading, shade_y;
};

struct text_style {
	int cell_w, cell_h, dx, dy, alphabet;
};

struct glyph_def {
	uint32_t cp;
	int nrows;
	uint32_t rows[MAX_ROWS];
};

struct alphabet {
	struct glyph_def *g;
	int n;
	char name[32];
	struct vt_font *font;
	uint32_t *codes, *rows;
};

struct regis {
	dmc_vt *t;
	uint32_t *palette;
	struct picture_layer layer;
	struct cps pending;
	int x, y;
	struct {
		int x, y, bounded;
	} stack[64];
	int nstack;
	struct writing w;
	struct text_style text, saved_text;
	int has_saved_text;
	int background;
	int ax1, ay1, ax2, ay2;
	struct cps macros[26];
	int has_macro[26];
	struct alphabet alpha[4];
	int loading;
	struct pts curve;
	int in_curve, curve_closed;
	struct pts *filling;
};

/* ------------------------------------------------------------------ */
/* the text of a command */

struct cursor {
	const uint32_t *s;
	int n, i;
};

static uint32_t peek(const struct cursor *c)
{
	return c->i < c->n ? c->s[c->i] : 0;
}

static void skip(struct cursor *c)
{
	while (c->i < c->n && (c->s[c->i] <= ' ' || c->s[c->i] == ','))
		c->i++;
}

static int is_digit(uint32_t c)
{
	return c >= '0' && c <= '9';
}

static int is_letter(uint32_t c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static uint32_t upper(uint32_t c)
{
	return c >= 'a' && c <= 'z' ? c - 32 : c;
}

/* a number: a sign, digits, a fraction (dropped); 0 if there's none */
static int number(struct cursor *c, int *v)
{
	int start = c->i, neg = 0, nd = 0;
	long x = 0;
	if (peek(c) == '+' || peek(c) == '-') {
		neg = peek(c) == '-';
		c->i++;
	}
	while (is_digit(peek(c))) {
		x = (x * 10 + (long)(peek(c) - '0')) % 1000000000L;
		c->i++;
		nd++;
	}
	if (!nd) {
		c->i = start;
		return 0;
	}
	if (peek(c) == '.') {
		c->i++;
		while (is_digit(peek(c)))
			c->i++;
	}
	*v = neg ? -(int)x : (int)x;
	return 1;
}

static void part(struct cursor *c, struct part *p)
{
	uint32_t ch;
	int v;
	while (peek(c) == ' ')
		c->i++;
	ch = peek(c);
	p->relative = ch == '+' || ch == '-';
	p->has = number(c, &v);
	p->value = p->has ? v : 0;
	while (c->i < c->n && c->s[c->i] != ',' && c->s[c->i] != ']')
		c->i++;
}

static void position(struct cursor *c, struct pos *p)
{
	int comma;
	c->i++;
	memset(p, 0, sizeof(*p));
	part(c, &p->x);
	comma = peek(c) == ',';
	if (comma) {
		c->i++;
		part(c, &p->y);
	}
	if (peek(c) == ']')
		c->i++;
	p->empty = !p->x.has && !p->y.has && !comma;
}

static void string(struct cursor *c, struct cps *out)
{
	uint32_t q = c->s[c->i++];
	while (c->i < c->n) {
		uint32_t ch = c->s[c->i++];
		if (ch == q) {
			if (peek(c) == q) {
				cps_add(out, q);
				c->i++;
			} else {
				break;
			}
		} else {
			cps_add(out, ch);
		}
	}
}

static void free_opts(struct opt *o, int n)
{
	int i;
	for (i = 0; i < n; i++) {
		free_opts(o[i].sub, o[i].nsub);
		cps_free(&o[i].str);
	}
	free(o);
}

static struct opt *group(struct cursor *c, int *n, int depth);

static void values(struct cursor *c, struct opt *o, int depth)
{
	uint32_t ch;
	while (peek(c) == ' ')
		c->i++;
	ch = peek(c);
	if (is_digit(ch) || ch == '+' || ch == '-') {
		int start = c->i, k = 0, j;
		o->has_num = number(c, &o->num);
		for (j = start; j < c->i && k < (int)sizeof(o->digits) - 1; j++)
			if (!(k == 0 && (c->s[j] == '+' || c->s[j] == '-')))
				o->digits[k++] = (char)c->s[j];
		o->digits[k] = 0;
		if (!o->has_num && c->i == start)
			c->i++;
	}
	for (;;) {
		while (peek(c) == ' ')
			c->i++;
		switch (peek(c)) {
		case '[':
			if (o->npos < MAX_POS)
				position(c, &o->positions[o->npos++]);
			else {
				struct pos dummy;
				position(c, &dummy);
			}
			break;
		case '(':
			free_opts(o->sub, o->nsub);
			o->sub = group(c, &o->nsub, depth + 1);
			break;
		case '\'': case '"':
			cps_free(&o->str);
			string(c, &o->str);
			o->has_str = 1;
			break;
		default:
			return;
		}
	}
}

static struct opt *group(struct cursor *c, int *n, int depth)
{
	struct opt *opts = NULL;
	int cap = 0, last = -1;
	*n = 0;
	c->i++;
	if (depth > 16) {
		/* (too deep: skipped to its end) */
		int d = 1;
		while (c->i < c->n && d > 0) {
			if (c->s[c->i] == '(')
				d++;
			else if (c->s[c->i] == ')')
				d--;
			c->i++;
		}
		return NULL;
	}
	while (c->i < c->n) {
		uint32_t ch;
		skip(c);
		ch = peek(c);
		if (ch == ')') {
			c->i++;
			break;
		}
		if (is_letter(ch) ||
		    ((is_digit(ch) || ch == '+' || ch == '-') && last >= 0)) {
			int letter = is_letter(ch) ? (int)upper(ch) : opts[last].letter;
			if (is_letter(ch))
				c->i++;
			if (*n == cap) {
				struct opt *no;
				cap = cap ? cap * 2 : 8;
				no = (struct opt *)realloc(opts, sizeof(*opts) * (size_t)cap);
				if (!no)
					break;
				opts = no;
			}
			memset(&opts[*n], 0, sizeof(opts[*n]));
			opts[*n].letter = letter;
			last = (*n)++;
			values(c, &opts[last], depth);
		} else if (ch == '[' && last >= 0) {
			if (opts[last].npos < MAX_POS) {
				position(c, &opts[last].positions[opts[last].npos++]);
			} else {
				struct pos dummy;
				position(c, &dummy);
			}
		} else if (ch == '(' && last >= 0) {
			free_opts(opts[last].sub, opts[last].nsub);
			opts[last].sub = group(c, &opts[last].nsub, depth + 1);
		} else if (ch == '\'' || ch == '"') {
			struct cps s;
			memset(&s, 0, sizeof(s));
			string(c, &s);
			if (last >= 0) {
				cps_free(&opts[last].str);
				opts[last].str = s;
				opts[last].has_str = 1;
			} else {
				cps_free(&s);
			}
		} else {
			c->i++;
		}
	}
	return opts;
}

enum { A_END, A_POS, A_GROUP, A_STRING, A_DIGIT };

/* the next argument of a command: a position, an option group, a string,
   a pixel vector's digit (anything else skipped) */
static int next_arg(struct cursor *c, struct pos *p, struct opt **g, int *ng,
		    struct cps *str, int *digit)
{
	for (;;) {
		uint32_t ch;
		skip(c);
		ch = peek(c);
		if (ch == 0 && c->i >= c->n)
			return A_END;
		if (ch == '[') {
			position(c, p);
			return A_POS;
		}
		if (ch == '(') {
			*g = group(c, ng, 0);
			return A_GROUP;
		}
		if (ch == '\'' || ch == '"') {
			memset(str, 0, sizeof(*str));
			string(c, str);
			return A_STRING;
		}
		c->i++;
		if (ch >= '0' && ch <= '7') {
			*digit = (int)(ch - '0');
			return A_DIGIT;
		}
	}
}

/* ------------------------------------------------------------------ */
/* coordinates */

static int map(int v, int a, int b, int size)
{
	int span = b == a ? 1 : b - a;
	return (int)((long long)(v - a) * (size - 1) / span);
}

static int dx(const struct regis *g, int ux)
{
	return map(ux, g->ax1, g->ax2, RW);
}

static int dy(const struct regis *g, int uy)
{
	return map(uy, g->ay1, g->ay2, RH);
}

static void user(const struct regis *g, int sx, int sy, int *ux, int *uy)
{
	*ux = g->ax1 + (int)((long long)sx * (g->ax2 - g->ax1) / (RW - 1));
	*uy = g->ay1 + (int)((long long)sy * (g->ay2 - g->ay1) / (RH - 1));
}

static void resolve(const struct regis *g, const struct pos *p, int *x, int *y)
{
	*x = !p->x.has ? g->x : p->x.relative ? g->x + p->x.value : p->x.value;
	*y = !p->y.has ? g->y : p->y.relative ? g->y + p->y.value : p->y.value;
}

static void step(const struct regis *g, int d, int *x, int *y)
{
	int m = g->w.multiplier, sx = dirs[d & 7][0], sy = dirs[d & 7][1];
	int ux = sx * m * (g->ax2 - g->ax1) / (RW - 1);
	int uy = sy * m * (g->ay2 - g->ay1) / (RH - 1);
	*x = g->x + (ux == 0 ? sx * m : ux);
	*y = g->y + (uy == 0 ? sy * m : uy);
}

static int roundi(double v)
{
	return (int)floor(v + 0.5);
}

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/* ------------------------------------------------------------------ */
/* drawing */

static uint32_t background_color(const struct regis *g)
{
	uint32_t c = g->palette[g->background & 15];
	return (c & 0xffffff) == 0 ? 0 : c | 0xff000000u;
}

static struct raster *setup(struct regis *g)
{
	struct raster *r = layer_raster(&g->layer);
	if (!r)
		return NULL;
	r->mode = g->w.mode;
	r->color = g->palette[g->w.color & 15] | 0xff000000u;
	r->background = background_color(g);
	r->pattern = g->w.negate ? ~g->w.pattern & 0xff : g->w.pattern;
	return r;
}

static void screen_line(struct regis *g, int x0, int y0, int x1, int y1)
{
	struct raster *r;
	if (g->filling) {
		pts_add(g->filling, x1, y1);
		return;
	}
	if (!(r = setup(g)))
		return;
	if (g->w.shading) {
		int sy = dy(g, g->w.shade_y), q[8];
		q[0] = x0; q[1] = y0; q[2] = x1; q[3] = y1;
		q[4] = x1; q[5] = sy; q[6] = x0; q[7] = sy;
		raster_fill(r, q, 4);
	}
	raster_line(r, x0, y0, x1, y1);
}

static void line_to(struct regis *g, int nx, int ny)
{
	screen_line(g, dx(g, g->x), dy(g, g->y), dx(g, nx), dy(g, ny));
	g->x = nx;
	g->y = ny;
}

static void dot(struct regis *g)
{
	struct raster *r;
	if (g->filling) {
		pts_add(g->filling, dx(g, g->x), dy(g, g->y));
		return;
	}
	if ((r = setup(g)) != NULL)
		raster_point(r, dx(g, g->x), dy(g, g->y));
}

static void round_(struct regis *g, int px, int py, int center, int has_arc,
		   int arc)
{
	int cx = center ? dx(g, px) : dx(g, g->x), cy = center ? dy(g, py) : dy(g, g->y);
	int ex = center ? dx(g, g->x) : dx(g, px), ey = center ? dy(g, g->y) : dy(g, py);
	double r = sqrt((double)(ex - cx) * (ex - cx) + (double)(ey - cy) * (ey - cy));
	double start = has_arc ? atan2(-(double)(ey - cy), (double)(ex - cx)) * 180 / M_PI : 0.0;
	double sweep = has_arc ? arc : 360;
	int i;
	if (g->filling) {
		for (i = 0; i <= 48; i++) {
			double a = (start + sweep * i / 48) * M_PI / 180;
			pts_add(g->filling, roundi(cx + r * cos(a)), roundi(cy - r * sin(a)));
		}
	} else {
		struct raster *ra = setup(g);
		if (ra)
			raster_arc(ra, cx, cy, roundi(r), start, sweep);
	}
	/* (an arc round a given centre leaves the position at its end) */
	if (center && has_arc) {
		double a = (start + sweep) * M_PI / 180;
		user(g, roundi(cx + r * cos(a)), roundi(cy - r * sin(a)), &g->x, &g->y);
	}
}

static void spline(struct regis *g, const struct pts *p, int closed)
{
	int n = p->n, spans, s, k, lx, ly;
	if (n < 2)
		return;
	spans = closed ? n : n - 1;
	lx = p->xy[0];
	ly = p->xy[1];
#define AT(i, c) (closed ? p->xy[2 * ((((i) % n) + n) % n) + (c)] : \
		  p->xy[2 * clampi((i), 0, n - 1) + (c)])
	for (s = 0; s < spans; s++) {
		for (k = 1; k <= STEPS; k++) {
			double u = (double)k / STEPS;
			int nx, ny, c;
			int v[2];
			for (c = 0; c < 2; c++) {
				double a = AT(s - 1, c), b = AT(s, c), cc = AT(s + 1, c), d = AT(s + 2, c);
				v[c] = roundi(0.5 * (2 * b + (-a + cc) * u +
						     (2 * a - 5 * b + 4 * cc - d) * u * u +
						     (-a + 3 * b - 3 * cc + d) * u * u * u));
			}
			nx = v[0];
			ny = v[1];
			screen_line(g, lx, ly, nx, ny);
			lx = nx;
			ly = ny;
		}
	}
#undef AT
}

/* ------------------------------------------------------------------ */
/* the colours */

static int color_of(const struct opt *o, int n, uint32_t *out)
{
	int h = -1, l = -1, s = -1, i;
	for (i = 0; i < n; i++) {
		int v = o[i].has_num ? o[i].num : 0;
		switch (o[i].letter) {
		case 'H': h = v; break;
		case 'L': l = v; break;
		case 'S': s = v; break;
		default:
			if (!o[i].has_num) {
				switch (o[i].letter) {
				case 'D': *out = 0xff000000u; return 1;
				case 'B': *out = 0xff0000ffu; return 1;
				case 'R': *out = 0xffff0000u; return 1;
				case 'G': *out = 0xff00ff00u; return 1;
				case 'M': *out = 0xffff00ffu; return 1;
				case 'C': *out = 0xff00ffffu; return 1;
				case 'Y': *out = 0xffffff00u; return 1;
				case 'W': *out = 0xffffffffu; return 1;
				}
			}
		}
	}
	if (h < 0 && l < 0 && s < 0)
		return 0;
	*out = sixel_hls(h < 0 ? 0 : h, l < 0 ? 50 : l, s < 0 ? (h < 0 ? 0 : 100) : s);
	return 1;
}

static int color_index(const struct regis *g, const struct opt *o, int *out)
{
	uint32_t c;
	int i, best = -1;
	long bd = 0;
	if (o->has_num) {
		*out = clampi(o->num, 0, 15);
		return 1;
	}
	if (!o->sub || !color_of(o->sub, o->nsub, &c))
		return 0;
	for (i = 0; i < 16; i++) {
		uint32_t p = g->palette[i];
		long dr = (long)((p >> 16) & 0xff) - (long)((c >> 16) & 0xff);
		long dg = (long)((p >> 8) & 0xff) - (long)((c >> 8) & 0xff);
		long db = (long)(p & 0xff) - (long)(c & 0xff);
		long d = dr * dr + dg * dg + db * db;
		if (best < 0 || d < bd) {
			best = i;
			bd = d;
		}
	}
	*out = best;
	return 1;
}

static void apply_writing(struct regis *g, const struct opt *o, int n,
			  struct writing *w)
{
	int i, k;
	for (i = 0; i < n; i++) {
		switch (o[i].letter) {
		case 'V': w->mode = R_OVERLAY; break;
		case 'R': w->mode = R_REPLACE; break;
		case 'C': w->mode = R_COMPLEMENT; break;
		case 'E': w->mode = R_ERASE; break;
		case 'I': color_index(g, &o[i], &w->color); break;
		case 'P': {
			const char *d = o[i].digits;
			int len = (int)strlen(d), ok = len > 1, bits = 0;
			for (k = 0; k < len; k++)
				if (d[k] != '0' && d[k] != '1')
					ok = 0;
			if (ok) {
				for (k = 0; k < 8; k++)
					bits = (bits << 1) | (d[k % len] - '0');
				w->pattern = bits;
			} else if (o[i].has_num) {
				w->pattern = patterns[clampi(o[i].num, 0, 9)];
			}
			break;
		}
		case 'N': w->negate = (o[i].has_num ? o[i].num : 1) != 0; break;
		case 'S':
			w->shading = o[i].has_str || (o[i].has_num ? o[i].num : 1) != 0;
			if (o[i].npos > 0) {
				int px, py;
				resolve(g, &o[i].positions[0], &px, &py);
				w->shade_y = py;
			} else {
				w->shade_y = g->y;
			}
			break;
		case 'M':
			if (o[i].has_num)
				w->multiplier = clampi(o[i].num, 1, 256);
			break;
		}
	}
}

/* ------------------------------------------------------------------ */
/* the alphabets */

static const struct vt_font *alpha_font(struct regis *g, int n)
{
	struct alphabet *a = &g->alpha[n];
	int i, j, h = 1, width = 8;
	uint32_t widest = 0;
	struct vt_font *f;
	if (a->font)
		return a->font;
	if (a->n <= 0)
		return NULL;
	for (i = 0; i < a->n; i++) {
		if (a->g[i].nrows > h)
			h = a->g[i].nrows;
		for (j = 0; j < a->g[i].nrows; j++)
			if (a->g[i].rows[j] > widest)
				widest = a->g[i].rows[j];
	}
	while (width < 24 && (widest >> width) != 0)
		width += 8;
	f = (struct vt_font *)calloc(1, sizeof(*f));
	a->codes = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)a->n);
	a->rows = (uint32_t *)calloc((size_t)a->n * (size_t)(h > 0 ? h : 1),
				      sizeof(uint32_t));
	if (!f || !a->codes || !a->rows) {
		free(f);
		free(a->codes);
		free(a->rows);
		a->codes = a->rows = NULL;
		return NULL;
	}
	/* (the glyphs are kept in code order as they're loaded) */
	for (i = 0; i < a->n; i++) {
		a->codes[i] = a->g[i].cp;
		for (j = 0; j < a->g[i].nrows; j++)
			a->rows[i * h + j] = a->g[i].rows[j];
	}
	f->w = width;
	f->h = h;
	f->n = a->n;
	f->codes = a->codes;
	f->rows32 = a->rows;
	a->font = f;
	return f;
}

static void alpha_drop_font(struct alphabet *a)
{
	free(a->font);
	free(a->codes);
	free(a->rows);
	a->font = NULL;
	a->codes = a->rows = NULL;
}

static void alpha_clear(struct alphabet *a)
{
	alpha_drop_font(a);
	free(a->g);
	a->g = NULL;
	a->n = 0;
	a->name[0] = 0;
}

static void alpha_put(struct alphabet *a, uint32_t cp, const uint32_t *rows, int n)
{
	int i, at;
	struct glyph_def *ng;
	alpha_drop_font(a);
	for (i = 0; i < a->n; i++)
		if (a->g[i].cp == cp)
			break;
	if (i == a->n) {
		if (a->n >= MAX_CHARS)
			return;
		ng = (struct glyph_def *)realloc(a->g, sizeof(*ng) * (size_t)(a->n + 1));
		if (!ng)
			return;
		a->g = ng;
		/* (kept sorted) */
		for (at = a->n; at > 0 && a->g[at - 1].cp > cp; at--)
			a->g[at] = a->g[at - 1];
		a->n++;
		i = at;
		a->g[i].cp = cp;
	}
	a->g[i].nrows = n;
	memcpy(a->g[i].rows, rows, sizeof(uint32_t) * (size_t)n);
}

static int hexv(uint32_t c)
{
	c = upper(c);
	if (c >= '0' && c <= '9')
		return (int)(c - '0');
	if (c >= 'A' && c <= 'F')
		return (int)(c - 'A' + 10);
	return -1;
}

/* a loaded character's rows: hex numbers, commas between */
static int hex_rows(struct cursor *c, uint32_t *rows)
{
	int n = 0;
	for (;;) {
		int start;
		uint32_t v = 0;
		skip(c);
		start = c->i;
		while (c->i < c->n && hexv(c->s[c->i]) >= 0) {
			v = ((v << 4) | (uint32_t)hexv(c->s[c->i])) & 0xffffff;
			c->i++;
		}
		if (c->i == start)
			return n;
		if (n < MAX_ROWS)
			rows[n++] = v;
	}
}

/* ------------------------------------------------------------------ */
/* the commands */

static int run(struct regis *g, const uint32_t *s, int n, int final, int depth);

static void cmd_position(struct regis *g, struct cursor *c, int *temp,
			 struct writing *saved)
{
	struct pos p;
	struct opt *o;
	struct cps str;
	int no, d, k, a;
	(void)saved;
	while ((a = next_arg(c, &p, &o, &no, &str, &d)) != A_END) {
		if (a == A_POS) {
			resolve(g, &p, &g->x, &g->y);
		} else if (a == A_DIGIT) {
			step(g, d, &g->x, &g->y);
		} else if (a == A_STRING) {
			cps_free(&str);
		} else if (a == A_GROUP) {
			for (k = 0; k < no; k++) {
				switch (o[k].letter) {
				case 'B': case 'S':
					if (g->nstack < 64) {
						g->stack[g->nstack].x = g->x;
						g->stack[g->nstack].y = g->y;
						g->stack[g->nstack].bounded = o[k].letter == 'B';
						g->nstack++;
					}
					break;
				case 'E':
					if (g->nstack > 0) {
						g->nstack--;
						g->x = g->stack[g->nstack].x;
						g->y = g->stack[g->nstack].y;
					}
					break;
				case 'W':
					if (o[k].sub)
						apply_writing(g, o[k].sub, o[k].nsub, &g->w);
					*temp = 1;
					break;
				}
			}
			free_opts(o, no);
		}
	}
}

static void cmd_vector(struct regis *g, struct cursor *c, int *temp)
{
	struct pos p;
	struct opt *o;
	struct cps str;
	int no, d, k, a, nx, ny;
	while ((a = next_arg(c, &p, &o, &no, &str, &d)) != A_END) {
		if (a == A_POS) {
			if (p.empty) {
				dot(g);
			} else {
				resolve(g, &p, &nx, &ny);
				line_to(g, nx, ny);
			}
		} else if (a == A_DIGIT) {
			step(g, d, &nx, &ny);
			line_to(g, nx, ny);
		} else if (a == A_STRING) {
			cps_free(&str);
		} else if (a == A_GROUP) {
			for (k = 0; k < no; k++) {
				switch (o[k].letter) {
				case 'B': case 'S':
					if (g->nstack < 64) {
						g->stack[g->nstack].x = g->x;
						g->stack[g->nstack].y = g->y;
						g->stack[g->nstack].bounded = o[k].letter == 'B';
						g->nstack++;
					}
					break;
				case 'E':
					if (g->nstack > 0) {
						g->nstack--;
						if (g->stack[g->nstack].bounded) {
							line_to(g, g->stack[g->nstack].x,
								g->stack[g->nstack].y);
						} else {
							g->x = g->stack[g->nstack].x;
							g->y = g->stack[g->nstack].y;
						}
					}
					break;
				case 'W':
					if (o[k].sub)
						apply_writing(g, o[k].sub, o[k].nsub, &g->w);
					*temp = 1;
					break;
				}
			}
			free_opts(o, no);
		}
	}
}

static void cmd_circle(struct regis *g, struct cursor *c, int *temp)
{
	struct pos p;
	struct opt *o;
	struct cps str;
	int no, d, k, a, px, py, center = 0, has_arc = 0, arc = 360;
	while ((a = next_arg(c, &p, &o, &no, &str, &d)) != A_END) {
		if (a == A_POS) {
			resolve(g, &p, &px, &py);
			if (g->in_curve) {
				pts_add(&g->curve, dx(g, px), dy(g, py));
				g->x = px;
				g->y = py;
			} else {
				round_(g, px, py, center, has_arc, arc);
			}
		} else if (a == A_DIGIT) {
			step(g, d, &px, &py);
			g->x = px;
			g->y = py;
			if (g->in_curve)
				pts_add(&g->curve, dx(g, px), dy(g, py));
		} else if (a == A_STRING) {
			cps_free(&str);
		} else if (a == A_GROUP) {
			for (k = 0; k < no; k++) {
				switch (o[k].letter) {
				case 'C': center = 1; break;
				case 'A':
					has_arc = 1;
					arc = o[k].has_num ? o[k].num : 360;
					break;
				case 'B': case 'S':
					g->curve.n = 0;
					g->in_curve = 1;
					pts_add(&g->curve, dx(g, g->x), dy(g, g->y));
					g->curve_closed = o[k].letter == 'B';
					break;
				case 'E':
					if (g->in_curve) {
						struct pts pc = g->curve;
						g->in_curve = 0;
						memset(&g->curve, 0, sizeof(g->curve));
						spline(g, &pc, g->curve_closed);
						pts_free(&pc);
					}
					break;
				case 'W':
					if (o[k].sub)
						apply_writing(g, o[k].sub, o[k].nsub, &g->w);
					*temp = 1;
					break;
				}
			}
			free_opts(o, no);
		}
	}
}

/* the text inside the parentheses at the cursor (balanced, quotes kept) */
static int parenthesised(struct cursor *c, int *start, int *len)
{
	int depth = 1;
	uint32_t quote = 0;
	skip(c);
	if (peek(c) != '(')
		return 0;
	*start = ++c->i;
	while (c->i < c->n) {
		uint32_t ch = c->s[c->i++];
		if (quote) {
			if (ch == quote)
				quote = 0;
		} else if (ch == '\'' || ch == '"') {
			quote = ch;
		} else if (ch == '(') {
			depth++;
		} else if (ch == ')' && --depth == 0) {
			*len = c->i - 1 - *start;
			return 1;
		}
	}
	*len = c->n - *start;
	return 1;
}

static void cmd_fill(struct regis *g, struct cursor *c, int depth)
{
	int start, len;
	struct pts pts, *outer;
	if (!parenthesised(c, &start, &len) || depth >= MAX_DEPTH)
		return;
	memset(&pts, 0, sizeof(pts));
	outer = g->filling;
	pts_add(&pts, dx(g, g->x), dy(g, g->y));
	g->filling = &pts;
	run(g, c->s + start, len, 1, depth + 1);
	g->filling = outer;
	if (outer) {
		int i;
		for (i = 0; i < pts.n; i++)
			pts_add(outer, pts.xy[2 * i], pts.xy[2 * i + 1]);
	} else {
		struct raster *r = setup(g);
		if (r)
			raster_fill(r, pts.xy, pts.n);
	}
	pts_free(&pts);
}

static void draw_string(struct regis *g, const struct cps *s)
{
	struct raster *r = setup(g);
	int px = dx(g, g->x), py = dy(g, g->y), start = px, i;
	const struct vt_font *font = g->text.alphabet > 0 ?
		alpha_font(g, g->text.alphabet) : NULL;
	for (i = 0; i < s->n; i++) {
		uint32_t ch = s->p[i];
		if (ch == '\r') {
			px = start;
		} else if (ch == '\n') {
			py += g->text.cell_h;
		} else {
			const struct vt_font *f = font && font_index(font, ch) >= 0 ?
				font : &vt_font_vga;
			if (!g->filling && ch != ' ' && r)
				raster_text(r, px, py, &ch, 1, f, g->text.cell_w, g->text.cell_h);
			px += g->text.dx * g->text.cell_w;
			py += g->text.dy * g->text.cell_h;
		}
	}
	user(g, px, py, &g->x, &g->y);
}

static void cmd_text(struct regis *g, struct cursor *c, int *temp)
{
	struct pos p;
	struct opt *o;
	struct cps str;
	int no, d, k, a;
	while ((a = next_arg(c, &p, &o, &no, &str, &d)) != A_END) {
		if (a == A_POS) {
			resolve(g, &p, &g->x, &g->y);
		} else if (a == A_STRING) {
			draw_string(g, &str);
			cps_free(&str);
		} else if (a == A_GROUP) {
			for (k = 0; k < no; k++) {
				struct opt *q = &o[k];
				switch (q->letter) {
				case 'S':
					if (q->npos > 0) {
						if (q->positions[0].x.has)
							g->text.cell_w = clampi(q->positions[0].x.value, 1, RW);
						if (q->positions[0].y.has)
							g->text.cell_h = clampi(q->positions[0].y.value, 1, RH);
					} else if (q->has_num) {
						int n = clampi(q->num, 0, 16);
						g->text.cell_w = n == 0 ? 10 : 10 * n;
						g->text.cell_h = n == 0 ? 10 : 20 * n;
					}
					break;
				case 'U': case 'M':
					if (q->npos > 0) {
						int unit = q->letter == 'M' ? 10 : 1;
						if (q->positions[0].x.has)
							g->text.cell_w = clampi(q->positions[0].x.value * unit, 1, RW);
						if (q->positions[0].y.has)
							g->text.cell_h = clampi(q->positions[0].y.value * unit, 1, RH);
					}
					break;
				case 'H':
					if (q->has_num)
						g->text.cell_h = clampi(10 * q->num, 1, RH);
					break;
				case 'D':
					if (q->has_num) {
						double r = q->num * M_PI / 180;
						g->text.dx = roundi(cos(r));
						g->text.dy = -roundi(sin(r));
					}
					break;
				case 'A':
					if (q->has_num)
						g->text.alphabet = clampi(q->num, 0, 3);
					break;
				case 'B':
					g->saved_text = g->text;
					g->has_saved_text = 1;
					break;
				case 'E':
					if (g->has_saved_text)
						g->text = g->saved_text;
					break;
				case 'W':
					if (q->sub)
						apply_writing(g, q->sub, q->nsub, &g->w);
					*temp = 1;
					break;
				}
			}
			free_opts(o, no);
		}
	}
}

static void cmd_writing(struct regis *g, struct cursor *c)
{
	struct pos p;
	struct opt *o;
	struct cps str;
	int no, d, a;
	while ((a = next_arg(c, &p, &o, &no, &str, &d)) != A_END) {
		if (a == A_GROUP) {
			apply_writing(g, o, no, &g->w);
			free_opts(o, no);
		} else if (a == A_STRING) {
			cps_free(&str);
		}
	}
}

static void cmd_screen(struct regis *g, struct cursor *c)
{
	struct pos p;
	struct opt *o;
	struct cps str;
	int no, d, a, k;
	while ((a = next_arg(c, &p, &o, &no, &str, &d)) != A_END) {
		if (a == A_STRING)
			cps_free(&str);
		if (a != A_GROUP)
			continue;
		for (k = 0; k < no; k++) {
			struct opt *q = &o[k];
			uint32_t col;
			switch (q->letter) {
			case 'E': {
				struct raster *r = setup(g);
				if (r)
					raster_clear(r, background_color(g));
				break;
			}
			case 'I':
				color_index(g, q, &g->background);
				break;
			case 'A':
				if (q->npos > 0) {
					if (q->positions[0].x.has)
						g->ax1 = q->positions[0].x.value;
					if (q->positions[0].y.has)
						g->ay1 = q->positions[0].y.value;
				}
				if (q->npos > 1) {
					if (q->positions[1].x.has)
						g->ax2 = q->positions[1].x.value;
					if (q->positions[1].y.has)
						g->ay2 = q->positions[1].y.value;
				}
				break;
			case 'M':
				if (q->has_num && q->sub && color_of(q->sub, q->nsub, &col) &&
				    q->num >= 0 && q->num < 16)
					g->palette[q->num] = col;
				break;
			}
		}
		free_opts(o, no);
	}
}

static void cmd_report(struct regis *g, struct cursor *c)
{
	struct pos p;
	struct opt *o;
	struct cps str;
	int no, d, a, k;
	char s[64];
	while ((a = next_arg(c, &p, &o, &no, &str, &d)) != A_END) {
		if (a == A_STRING)
			cps_free(&str);
		if (a != A_GROUP)
			continue;
		for (k = 0; k < no; k++) {
			switch (o[k].letter) {
			case 'P':
				snprintf(s, sizeof(s), "[%d,%d]\r", g->x, g->y);
				vt_reply(g->t, s);
				break;
			case 'E':
				vt_reply(g->t, "\"0,0\"\r");
				break;
			case 'L':
				snprintf(s, sizeof(s), "A%d\"%s\"\r", g->loading,
					 g->alpha[g->loading].name);
				vt_reply(g->t, s);
				break;
			}
		}
		free_opts(o, no);
	}
}

static void cmd_load(struct regis *g, struct cursor *c)
{
	struct pos p;
	struct opt *o;
	struct cps str;
	int no, d, a, k, i;
	uint32_t rows[MAX_ROWS];
	while ((a = next_arg(c, &p, &o, &no, &str, &d)) != A_END) {
		if (a == A_GROUP) {
			for (k = 0; k < no; k++)
				if (o[k].letter == 'A') {
					if (o[k].has_num)
						g->loading = clampi(o[k].num, 1, 3);
					if (o[k].has_str) {
						struct alphabet *al = &g->alpha[g->loading];
						for (i = 0; i < o[k].str.n && i < (int)sizeof(al->name) - 1; i++)
							al->name[i] = o[k].str.p[i] < 0x80 ? (char)o[k].str.p[i] : '?';
						al->name[i] = 0;
					}
				}
			free_opts(o, no);
		} else if (a == A_STRING) {
			if (str.n > 0) {
				int nr = hex_rows(c, rows);
				alpha_put(&g->alpha[g->loading], str.p[0], rows, nr);
			}
			cps_free(&str);
		}
	}
}

static int is_command(uint32_t c)
{
	c = upper(c);
	return c && c < 0x80 && strchr("PVCFTWSRL", (int)c) != NULL;
}

/* one command (its letter first, in capitals) */
static void command(struct regis *g, const uint32_t *s, int n, int depth)
{
	struct cursor c;
	struct writing saved = g->w;
	int temp = 0;
	c.s = s;
	c.n = n;
	c.i = 1;
	switch (s[0]) {
	case 'P': cmd_position(g, &c, &temp, &saved); break;
	case 'V': cmd_vector(g, &c, &temp); break;
	case 'C': cmd_circle(g, &c, &temp); break;
	case 'F': cmd_fill(g, &c, depth); break;
	case 'T': cmd_text(g, &c, &temp); break;
	case 'W': cmd_writing(g, &c); break;
	case 'S': cmd_screen(g, &c); break;
	case 'R': cmd_report(g, &c); break;
	case 'L': cmd_load(g, &c); break;
	}
	if (temp)
		g->w = saved;
}

/* where the command at start ends: the next command (outside quotes,
   brackets and parentheses), or -1 if none has begun */
static int command_end(const uint32_t *s, int n, int start)
{
	int hex = upper(s[start]) == 'L', parens = 0, brackets = 0, j;
	uint32_t quote = 0;
	for (j = start + 1; j < n; j++) {
		uint32_t c = s[j];
		if (quote) {
			if (c == quote)
				quote = 0;
		} else if (c == '\'' || c == '"') {
			quote = c;
		} else if (c == '(') {
			parens++;
		} else if (c == ')') {
			if (parens > 0)
				parens--;
		} else if (c == '[') {
			brackets++;
		} else if (c == ']') {
			if (brackets > 0)
				brackets--;
		} else if (parens > 0 || brackets > 0) {
			;
		} else if (c == ';' || c == '@') {
			return j;
		} else if (hex && upper(c) >= 'A' && upper(c) <= 'F') {
			;
		} else if (is_command(c)) {
			return j;
		}
	}
	return -1;
}

static int find_close(const uint32_t *s, int n, int from)
{
	int j;
	for (j = from; j + 1 < n; j++)
		if (s[j] == '@' && s[j + 1] == ';')
			return j;
	return -1;
}

static int macrograph(struct regis *g, const uint32_t *s, int n, int i,
		      int final, int depth)
{
	uint32_t c;
	int close, k;
	if (i + 1 >= n)
		return final ? n : -1;
	c = s[i + 1];
	if (c == ':') {
		if (i + 2 >= n)
			return final ? n : -1;
		close = find_close(s, n, i + 3);
		if (close < 0)
			return final ? n : -1;
		k = (int)upper(s[i + 2]) - 'A';
		if (k >= 0 && k < 26) {
			struct cps *m = &g->macros[k];
			int j;
			m->n = 0;
			for (j = i + 3; j < close; j++)
				cps_add(m, s[j]);
			g->has_macro[k] = 1;
		}
		return close + 2;
	}
	if (c == '.') {
		for (k = 0; k < 26; k++) {
			cps_free(&g->macros[k]);
			g->has_macro[k] = 0;
		}
		return i + 2;
	}
	k = (int)upper(c) - 'A';
	if (depth < MAX_DEPTH && k >= 0 && k < 26 && g->has_macro[k]) {
		/* (a copy: the macro may redefine itself) */
		struct cps m;
		int j;
		memset(&m, 0, sizeof(m));
		for (j = 0; j < g->macros[k].n; j++)
			cps_add(&m, g->macros[k].p[j]);
		run(g, m.p, m.n, 1, depth + 1);
		cps_free(&m);
	}
	return i + 2;
}

static int run(struct regis *g, const uint32_t *s, int n, int final, int depth)
{
	int i = 0;
	while (i < n) {
		uint32_t c = s[i];
		int e, end, k;
		uint32_t *cmd;
		uint32_t quote = 0;
		if (c == '@') {
			int used = macrograph(g, s, n, i, final, depth);
			if (used < 0)
				return i;
			i = used;
			continue;
		}
		if (!is_command(c)) {
			i++;
			continue;
		}
		e = command_end(s, n, i);
		if (e < 0 && !final)
			return i;
		end = e < 0 ? n : e;
		/* the command in capitals but for its quoted strings */
		cmd = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)(end - i));
		if (!cmd)
			return n;
		for (k = 0; k < end - i; k++) {
			uint32_t ch = s[i + k];
			if (quote) {
				if (ch == quote)
					quote = 0;
				cmd[k] = ch;
			} else if (ch == '\'' || ch == '"') {
				quote = ch;
				cmd[k] = ch;
			} else {
				cmd[k] = upper(ch);
			}
		}
		command(g, cmd, end - i, depth);
		free(cmd);
		i = end;
	}
	return i;
}

static void run_pending(struct regis *g, int final)
{
	int used;
	if (!g->pending.n)
		return;
	used = run(g, g->pending.p, g->pending.n, final, 0);
	if (used >= g->pending.n) {
		g->pending.n = 0;
	} else if (used > 0) {
		memmove(g->pending.p, g->pending.p + used,
			sizeof(uint32_t) * (size_t)(g->pending.n - used));
		g->pending.n -= used;
	}
}

/* ------------------------------------------------------------------ */
/* the API */

static void restart(struct regis *g)
{
	g->x = 0;
	g->y = 0;
	g->nstack = 0;
	g->w.mode = R_OVERLAY;
	g->w.color = 7;
	g->w.pattern = 0xff;
	g->w.negate = 0;
	g->w.multiplier = 1;
	g->w.shading = 0;
	g->w.shade_y = 0;
	g->text.cell_w = 10;
	g->text.cell_h = 20;
	g->text.dx = 1;
	g->text.dy = 0;
	g->text.alphabet = 0;
	g->has_saved_text = 0;
	g->ax1 = 0;
	g->ay1 = 0;
	g->ax2 = RW - 1;
	g->ay2 = RH - 1;
	g->in_curve = 0;
	g->curve.n = 0;
	g->filling = NULL;
	g->loading = 1;
}

struct regis *regis_new(dmc_vt *t, uint32_t *registers)
{
	struct regis *g = (struct regis *)calloc(1, sizeof(*g));
	if (!g)
		return NULL;
	g->t = t;
	g->palette = registers;
	layer_init(&g->layer, t, RW, RH, NULL, NULL);
	restart(g);
	return g;
}

void regis_begin(struct regis *g, int p1)
{
	g->pending.n = 0;
	if (p1 == 1 || p1 == 3)
		restart(g);
}

void regis_feed(struct regis *g, int32_t cp)
{
	if (g->pending.n < MAX_PENDING)
		cps_add(&g->pending, (uint32_t)cp);
}

void regis_flush(struct regis *g)
{
	run_pending(g, 0);
	layer_flush(&g->layer);
}

void regis_end(struct regis *g)
{
	run_pending(g, 1);
}

void regis_abort(struct regis *g)
{
	g->pending.n = 0;
}

void regis_reset(struct regis *g)
{
	int k;
	g->pending.n = 0;
	for (k = 0; k < 26; k++) {
		cps_free(&g->macros[k]);
		g->has_macro[k] = 0;
	}
	for (k = 0; k < 4; k++)
		alpha_clear(&g->alpha[k]);
	g->background = 0;
	restart(g);
	layer_reset(&g->layer);
}

void regis_free(struct regis *g)
{
	int k;
	if (!g)
		return;
	cps_free(&g->pending);
	for (k = 0; k < 26; k++)
		cps_free(&g->macros[k]);
	for (k = 0; k < 4; k++)
		alpha_clear(&g->alpha[k]);
	pts_free(&g->curve);
	layer_free(&g->layer);
	free(g);
}
