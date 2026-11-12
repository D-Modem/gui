/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * vt_dec.c - the DEC VT line's sequences, and xterm's where the profile
 * has them (the app's VtInterpreter): a parser of ESC, CSI, OSC, DCS and
 * the strings as DEC's terminals read them, and what each does on the
 * screen.  With it what grew from it: a VT420's and VT5xx's additions
 * (Vt420Interpreter, Rectangles), G0-G3 and their shifts
 * (CharsetShifting), soft fonts (SoftFont), user-defined keys (UserKeys),
 * the printer port (PrinterPort); and the PC BBS terminals built on it:
 * ANSI-BBS as SyncTERM's CTerm is (AnsiBbsInterpreter, AnsiMusic, its
 * fonts), Avatar (AvatarInterpreter) and RIPscrip's text side.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vtint.h"

#define SOFT_BASE 0xf0000
#define SOFT_MAX 24

/* ------------------------------------------------------------------ */
/* the printer port */

void printer_init(struct vt_printer *p, dmc_vt *t, const char *end1,
		  const char *end2)
{
	memset(p, 0, sizeof(*p));
	p->t = t;
	p->ends[0] = end1;
	p->ends[1] = end2;
}

void printer_start(struct vt_printer *p)
{
	p->controller = 1;
	buf_clear(&p->buf);
}

/* the first n bytes held, to the printer */
static void printer_flush(struct vt_printer *p, int n)
{
	char *s;
	if (n <= 0)
		return;
	s = dup_n(p->buf.p, (size_t)n);
	if (s) {
		vt_print_out(p->t, s);
		free(s);
	}
	buf_drop(&p->buf, n);
}

static int ends_with(const dmc_buf *b, const char *e)
{
	int n = (int)strlen(e);
	return b->n >= n && memcmp(b->p + b->n - n, e, (size_t)n) == 0;
}

void printer_feed(struct vt_printer *p, int32_t cp)
{
	int i, n;
	utf8_put(&p->buf, (uint32_t)cp);
	for (i = 0; i < 2; i++) {
		const char *e = p->ends[i];
		if (e && ends_with(&p->buf, e)) {
			p->buf.n -= (int)strlen(e);
			p->buf.p[p->buf.n] = 0;
			printer_flush(p, p->buf.n);
			p->controller = 0;
			return;
		}
	}
	/* (a long job sent on in parts, all but what could start an end) */
	if (p->buf.n >= 4096) {
		n = p->buf.n - 8;
		while (n > 0 && ((unsigned char)p->buf.p[n] & 0xc0) == 0x80)
			n--;
		printer_flush(p, n);
	}
}

void printer_line(struct vt_printer *p, int y)
{
	dmc_buf b;
	struct vt_line *l = vt_line(p->t, y);
	memset(&b, 0, sizeof(b));
	line_text(l, 0, l->cols, &b);
	buf_addc(&b, '\n');
	vt_print_out(p->t, buf_str(&b));
	buf_free(&b);
}

void printer_screen(struct vt_printer *p)
{
	dmc_buf b;
	int y;
	memset(&b, 0, sizeof(b));
	for (y = 0; y < p->t->rows; y++) {
		struct vt_line *l = vt_line(p->t, y);
		line_text(l, 0, l->cols, &b);
		buf_addc(&b, '\n');
	}
	vt_print_out(p->t, buf_str(&b));
	buf_free(&b);
}

void printer_reset(struct vt_printer *p)
{
	p->controller = 0;
	p->autop = 0;
	buf_clear(&p->buf);
}

void printer_free(struct vt_printer *p)
{
	buf_free(&p->buf);
}

/* ------------------------------------------------------------------ */
/* rectangular areas (a VT420's, and Avatar's) */

struct area {
	int top, left, bottom, right;
};

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static void rect_copy(dmc_vt *t, const struct area *a, int to_y, int to_x)
{
	int h = a->bottom - a->top + 1, w = a->right - a->left + 1, x, y;
	int32_t *ch;
	vt_style *st;
	if (h > t->rows - to_y)
		h = t->rows - to_y;
	if (w > t->cols - to_x)
		w = t->cols - to_x;
	if (h <= 0 || w <= 0 || to_y < 0 || to_x < 0)
		return;
	/* (taken first, so an area copied over itself is right) */
	ch = (int32_t *)malloc(sizeof(int32_t) * (size_t)w * h);
	st = (vt_style *)malloc(sizeof(vt_style) * (size_t)w * h);
	if (!ch || !st) {
		free(ch);
		free(st);
		return;
	}
	for (y = 0; y < h; y++) {
		struct vt_line *l = vt_line(t, a->top + y);
		for (x = 0; x < w; x++) {
			ch[y * w + x] = l->ch[a->left + x];
			st[y * w + x] = l->st[a->left + x];
		}
	}
	for (y = 0; y < h; y++) {
		struct vt_line *l = vt_line(t, to_y + y);
		for (x = 0; x < w; x++)
			line_set(l, to_x + x, ch[y * w + x], st[y * w + x]);
	}
	free(ch);
	free(st);
	t->changed = 1;
}

static void rect_erase(dmc_vt *t, const struct area *a, int selective)
{
	vt_style e = st_erased(t->style);
	int x, y;
	for (y = a->top; y <= a->bottom; y++) {
		struct vt_line *l = vt_line(t, y);
		for (x = a->left; x <= a->right; x++) {
			if (selective && (st_attrs(l->st[x]) & A_PROTECTED))
				continue;
			line_set(l, x, 0, e);
		}
	}
	t->changed = 1;
}

static void rect_fill(dmc_vt *t, const struct area *a, int32_t cp)
{
	int x, y;
	for (y = a->top; y <= a->bottom; y++) {
		struct vt_line *l = vt_line(t, y);
		for (x = a->left; x <= a->right; x++)
			line_set(l, x, cp, t->style);
	}
	t->changed = 1;
}

#define CHANGEABLE (A_BOLD | A_UNDERLINE | A_BLINK | A_INVERSE | A_INVISIBLE)

static void rect_attributes(dmc_vt *t, const struct area *a, const int *ps,
			    int n, int reverse, int rectangle)
{
	int on = 0, off = 0, i, x, y;
	for (i = 0; i < (n > 0 ? n : 1); i++) {
		switch (n > 0 ? ps[i] : 0) {
		case 0: off |= CHANGEABLE; break;
		case 1: on |= A_BOLD; break;
		case 4: on |= A_UNDERLINE; break;
		case 5: on |= A_BLINK; break;
		case 7: on |= A_INVERSE; break;
		case 8: on |= A_INVISIBLE; break;
		case 22: off |= A_BOLD; break;
		case 24: off |= A_UNDERLINE; break;
		case 25: off |= A_BLINK; break;
		case 27: off |= A_INVERSE; break;
		case 28: off |= A_INVISIBLE; break;
		}
	}
	for (y = a->top; y <= a->bottom; y++) {
		struct vt_line *l = vt_line(t, y);
		int from = rectangle || y == a->top ? a->left : 0;
		int to = rectangle || y == a->bottom ? a->right : t->cols - 1;
		for (x = from; x <= to; x++) {
			vt_style s = l->st[x];
			int at = st_attrs(s);
			at = reverse ? at ^ ((on | off) & CHANGEABLE) : (at & ~off) | on;
			l->st[x] = st_with_attrs(s, at);
		}
	}
	t->changed = 1;
}

static int rect_checksum(dmc_vt *t, const struct area *a)
{
	int sum = 0, x, y;
	for (y = a->top; y <= a->bottom; y++) {
		struct vt_line *l = vt_line(t, y);
		for (x = a->left; x <= a->right; x++) {
			int cp = l->ch[x] <= 0 ? ' ' : l->ch[x];
			int at = st_attrs(l->st[x]);
			sum += cp;
			if (at & A_PROTECTED) sum += 0x04;
			if (at & A_INVISIBLE) sum += 0x08;
			if (at & A_ANY_UNDER) sum += 0x10;
			if (at & A_INVERSE) sum += 0x20;
			if (at & A_BLINK) sum += 0x40;
			if (at & A_BOLD) sum += 0x80;
		}
	}
	return (-sum) & 0xffff;
}

static void insert_columns(dmc_vt *t, int at, int n)
{
	vt_style e = st_erased(t->style);
	int y;
	for (y = t->top; y <= t->bottom; y++)
		line_insert_cells(vt_line(t, y), at, n, e, t->right + 1);
	t->changed = 1;
}

static void delete_columns(dmc_vt *t, int at, int n)
{
	vt_style e = st_erased(t->style);
	int y;
	for (y = t->top; y <= t->bottom; y++)
		line_delete_cells(vt_line(t, y), at, n, e, t->right + 1);
	t->changed = 1;
}

/* ------------------------------------------------------------------ */
/* ANSI music */

void music_reset(struct ansi_music *m)
{
	m->tempo = 120;
	m->octave = 4;
	m->length = 4;
	m->sounding = 7.0 / 8;
}

static int note_ms(const struct ansi_music *m, int len, double dot)
{
	return (int)floor(60000.0 / m->tempo * 4 / len * dot + 0.5);
}

static void music_note(dmc_vt *t, struct ansi_music *m, int n, int len,
		       double dot)
{
	int ms = note_ms(m, len, dot);
	int on = (int)floor(ms * m->sounding + 0.5);
	vt_add_music(t, 440.0 * pow(2.0, (n - 45) / 12.0), on, ms - on);
}

static int is_digit(int c)
{
	return c >= '0' && c <= '9';
}

/* a number at *i, or -1 (*i past it) */
static int music_number(const char *u, int *i)
{
	int v = 0, any = 0;
	while (is_digit(u[*i])) {
		if (v < 100000)
			v = v * 10 + (u[*i] - '0');
		(*i)++;
		any = 1;
	}
	return any ? v : -1;
}

static double music_dots(const char *u, int *i)
{
	double f = 1.0, add = 0.5;
	while (u[*i] == '.') {
		f += add;
		add /= 2;
		(*i)++;
	}
	return f;
}

void music_play(dmc_vt *t, struct ansi_music *m, const char *s)
{
	static const int semitones[7] = { 9, 11, 0, 2, 4, 5, 7 };
	char *u = dup_str(s);
	int i = 0, k, n, len;
	double dot;
	if (!u)
		return;
	for (k = 0; u[k]; k++)
		if (u[k] >= 'a' && u[k] <= 'z')
			u[k] = (char)(u[k] - 32);
	while (u[i]) {
		char c = u[i++];
		if (c >= 'A' && c <= 'G') {
			n = m->octave * 12 + semitones[c - 'A'];
			if (u[i] == '#' || u[i] == '+') {
				n++;
				i++;
			} else if (u[i] == '-') {
				n--;
				i++;
			}
			len = music_number(u, &i);
			if (len < 1 || len > 64)
				len = m->length;
			dot = music_dots(u, &i);
			music_note(t, m, clampi(n, 0, 83), len, dot);
		} else if (c == 'N') {
			n = music_number(u, &i);
			if (n < 0)
				continue;
			dot = music_dots(u, &i);
			if (n == 0)
				vt_add_music(t, 0, 0, note_ms(m, m->length, dot));
			else if (n <= 84)
				music_note(t, m, n - 1, m->length, dot);
		} else if (c == 'P') {
			len = music_number(u, &i);
			if (len < 1 || len > 64)
				len = m->length;
			vt_add_music(t, 0, 0, note_ms(m, len, music_dots(u, &i)));
		} else if (c == 'O') {
			n = music_number(u, &i);
			if (n >= 0)
				m->octave = clampi(n, 0, 6);
		} else if (c == '>') {
			if (m->octave < 6)
				m->octave++;
		} else if (c == '<') {
			if (m->octave > 0)
				m->octave--;
		} else if (c == 'L') {
			n = music_number(u, &i);
			if (n >= 1 && n <= 64)
				m->length = n;
		} else if (c == 'T') {
			n = music_number(u, &i);
			if (n >= 32 && n <= 255)
				m->tempo = n;
		} else if (c == 'M') {
			if (u[i]) {
				if (u[i] == 'N')
					m->sounding = 7.0 / 8;
				else if (u[i] == 'L')
					m->sounding = 1.0;
				else if (u[i] == 'S')
					m->sounding = 3.0 / 4;
				i++;
			}
		}
	}
	free(u);
}

/* ------------------------------------------------------------------ */
/* the interpreter's state */

/* a soft character set the host loaded (DECDLD) */
struct soft_font {
	char designator[8];
	int has_designator;
	int is96, w, h;
	uint8_t present[96];
	uint32_t glyphs[96][SOFT_MAX];
	/* the glyphs as a font of SOFT_BASE's code points */
	struct vt_font font;
	uint32_t codes[96];
	uint32_t rows[96 * SOFT_MAX];
};

enum {
	S_GROUND, S_ESC, S_ESC_INTER, S_CSI, S_OSC, S_OSC_ESC, S_STRING,
	S_STRING_ESC, S_DCS, S_DCS_PASS, S_DCS_ESC
};

#define NPARAMS 32
#define NUDK 35

/* the ^V commands' argument counts (Avatar) */
#define AV_NONE (-1)
#define AV_LEAD (-2)
#define AV_REPEAT (-3)

struct dec {
	struct vt_interp in;
	const struct vt_profile *p;
	int vt420, bbs, avatar;

	/* G0-G3, GL, GR, the single shift and the preferred set */
	uint8_t g[4];
	int gl, gr, single, preferred;
	struct soft_font soft;

	/* the user-defined keys (DECUDK) */
	dmc_buf udk[NUDK];
	uint8_t has_udk[NUDK];
	int udk_locked;

	int level;
	char designation[4];
	int status_active;
	struct vt_interp *sub;
	int sub_done, sub_tek;
	struct vt_printer printer;
	int allow_columns;

	/* the parser */
	int state;
	int params[NPARAMS];
	uint8_t colon[NPARAMS];
	int nparams, marker, inter;
	dmc_buf osc;
	int osc_len, osc_bel;

	/* a DCS: a sixel image, ReGIS, or a request's text */
	struct sixel *sixel;
	uint32_t *registers;
	struct regis *regis;
	int in_regis;
	int dcs_on;
	dmc_buf dcs;
	int dcs_len;
	char dcs_kind[4];
	int dcs_p0, dcs_p1, dcs_params[NPARAMS], n_dcs_params;

	/* a VT420's */
	int rectangle, backarrow_bs;

	/* ANSI-BBS's */
	int music_mode, maybe_music, doorway, in_music;
	dmc_buf music;
	struct ansi_music player;

	/* Avatar's */
	int av_cmd, av_need, av_quoted, av_n;
	int av_args[260];

	/* RIPscrip's */
	struct rip *rip;
};

#define T(d) ((d)->in.t)
#define HAS(d, f) (((d)->p->features & (f)) != 0)
#define XTERM(d) HAS(d, F_XTERM)

static void process(struct vt_interp *in, int32_t cp);
static void esc(struct dec *d, int32_t cp);
static void dec_reset(struct vt_interp *in);

/* parameter i as it came (0 when missing) */
static int param(const struct dec *d, int i)
{
	return i < d->nparams ? d->params[i] : 0;
}

/* parameter i, or def when missing or 0 */
static int p(const struct dec *d, int i, int def)
{
	return i < d->nparams && d->params[i] != 0 ? d->params[i] : def;
}

/* ------------------------------------------------------------------ */
/* the character sets */

static void sets_reset(struct dec *d)
{
	d->g[0] = G_ASCII;
	d->g[1] = G_ASCII;
	d->g[2] = (uint8_t)d->preferred;
	d->g[3] = (uint8_t)d->preferred;
	d->gl = 0;
	d->gr = 2;
	d->single = -1;
}

static uint32_t soft_map(const struct soft_font *s, uint32_t cp)
{
	return cp >= 0x20 && cp <= 0x7f && s->present[cp - 0x20] ?
		SOFT_BASE + cp : cp;
}

static uint32_t show(const struct dec *d, int set, uint32_t cp)
{
	return set == G_SOFT ? soft_map(&d->soft, cp) : gset_map(set, cp);
}

static uint32_t sets_map(struct dec *d, uint32_t cp, int eight_bit)
{
	int s = d->single;
	d->single = -1;
	if (cp >= 0x20 && cp <= 0x7f)
		return show(d, d->g[s >= 0 ? s : d->gl], cp);
	if (eight_bit && cp >= 0xa0 && cp <= 0xff)
		return show(d, d->g[s >= 0 ? s : d->gr], cp - 0x80);
	return cp;
}

static const char *set_designator(const struct dec *d, int n)
{
	const char *s;
	if (d->g[n] == G_SOFT)
		return d->soft.has_designator ? d->soft.designator : "B";
	s = gset_designator(d->g[n]);
	return s && *s ? s : "B";
}

static int set_is96(const struct dec *d, int n)
{
	return d->g[n] == G_SOFT ? d->soft.is96 : gset_is96(d->g[n]);
}

static int save_sets(struct vt_interp *in, struct vt_sets_saved *s)
{
	struct dec *d = (struct dec *)in;
	memcpy(s->g, d->g, 4);
	s->gl = d->gl;
	s->gr = d->gr;
	return 1;
}

static void restore_sets(struct vt_interp *in, const struct vt_sets_saved *s)
{
	struct dec *d = (struct dec *)in;
	if (!s) {
		sets_reset(d);
		return;
	}
	memcpy(d->g, s->g, 4);
	d->gl = s->gl;
	d->gr = s->gr;
	d->single = -1;
}

/* ------------------------------------------------------------------ */
/* the soft font (DECDLD) */

static void soft_build(dmc_vt *t, struct soft_font *s)
{
	int c, n = 0, r;
	for (c = 0; c < 96; c++) {
		if (!s->present[c])
			continue;
		s->codes[n] = SOFT_BASE + 0x20 + c;
		for (r = 0; r < s->h; r++)
			s->rows[n * s->h + r] = s->glyphs[c][r];
		n++;
	}
	s->font.w = s->w;
	s->font.h = s->h;
	s->font.n = n;
	s->font.codes = s->codes;
	s->font.rows8 = NULL;
	s->font.rows32 = s->rows;
	t->soft_glyphs = n ? &s->font : NULL;
	t->changed = 1;
}

static void soft_reset(dmc_vt *t, struct soft_font *s)
{
	memset(s->present, 0, sizeof(s->present));
	s->has_designator = 0;
	s->designator[0] = 0;
	if (t->soft_glyphs == &s->font)
		t->soft_glyphs = NULL;
}

static void soft_glyph(const char *g, int len, int w, int h, uint32_t *rows)
{
	int band = 0, col = 0, i, b;
	memset(rows, 0, sizeof(uint32_t) * SOFT_MAX);
	for (i = 0; i < len; i++) {
		char c = g[i];
		if (c == '/') {
			band++;
			col = 0;
		} else if (c >= '?' && c <= '~') {
			int bits = c - 0x3f;
			if (col < w)
				for (b = 0; b < 6; b++) {
					int y = band * 6 + b;
					if ((bits & (1 << b)) && y < h)
						rows[y] |= 1u << (w - 1 - col);
				}
			col++;
		}
	}
}

static void soft_load(struct dec *d, const int *ps, int np, const char *data)
{
	struct soft_font *s = &d->soft;
	int i = 0, w, h, ch, m;
	char dscs[8];
	const char *g;
#define PP(k) ((k) < np ? ps[k] : 0)
	while (data[i] && (unsigned char)data[i] >= 0x20 &&
	       (unsigned char)data[i] <= 0x2f)
		i++;
	if (!data[i] || (unsigned char)data[i] < 0x30 ||
	    (unsigned char)data[i] > 0x7e || i + 1 >= (int)sizeof(dscs))
		return;
	memcpy(dscs, data, (size_t)i + 1);
	dscs[i + 1] = 0;
	m = PP(3);
	switch (m) {
	case 0: w = PP(4) == 2 ? 9 : 15; break;
	case 1: w = 15; break;
	case 2: w = 5; break;
	case 3: w = 6; break;
	case 4: w = 7; break;
	default: w = m; break;
	}
	w = clampi(w, 1, SOFT_MAX);
	/* (a VT320's cell is 15x12 at 80 columns, a VT220's 10 high) */
	h = clampi(PP(6) == 0 ? (w >= 15 ? 12 : 10) : PP(6), 1, SOFT_MAX);
	/* Pe: 0 and 2 every character erased first, 1 only those loaded */
	if (PP(2) != 1 || !s->has_designator || strcmp(dscs, s->designator) ||
	    w != s->w || h != s->h)
		memset(s->present, 0, sizeof(s->present));
	strcpy(s->designator, dscs);
	s->has_designator = 1;
	s->is96 = PP(7) == 1;
	s->w = w;
	s->h = h;
	/* Pcn: the first character's place (a 94-character set's from 1) */
	ch = 0x20 + (PP(1) > (s->is96 ? 0 : 1) ? PP(1) : (s->is96 ? 0 : 1));
	g = data + i + 1;
	for (;;) {
		const char *e = strchr(g, ';');
		int len = e ? (int)(e - g) : (int)strlen(g);
		if (ch > 0x7f || (!s->is96 && ch > 0x7e))
			break;
		soft_glyph(g, len, w, h, s->glyphs[ch - 0x20]);
		s->present[ch - 0x20] = 1;
		ch++;
		if (!e)
			break;
		g = e + 1;
	}
	soft_build(T(d), s);
#undef PP
}

static int soft_names(const struct soft_font *s, const char *d, int is96)
{
	return s->has_designator && !strcmp(s->designator, d) && s->is96 == is96;
}

/* ------------------------------------------------------------------ */
/* the user-defined keys (DECUDK) */

static int udk_valid(int k)
{
	switch (k) {
	case 17: case 18: case 19: case 20: case 21: case 23: case 24:
	case 25: case 26: case 28: case 29: case 31: case 32: case 33: case 34:
		return 1;
	}
	return 0;
}

static int hexv(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static void udk_load(struct dec *d, int clear, int lock, const char *data)
{
	const char *s = data;
	int i;
	if (d->udk_locked)
		return;
	if (clear == 0)
		for (i = 0; i < NUDK; i++) {
			d->has_udk[i] = 0;
			buf_clear(&d->udk[i]);
		}
	while (*s) {
		const char *e = strchr(s, ';'), *slash;
		int len = e ? (int)(e - s) : (int)strlen(s), key = 0, ok = 1, n;
		char *def = dup_n(s, (size_t)len);
		s = e ? e + 1 : s + len;
		if (!def)
			continue;
		slash = strchr(def, '/');
		if (!slash || slash == def) {
			free(def);
			continue;
		}
		for (i = 0; def + i < slash; i++) {
			if (!is_digit(def[i]) || key > 1000) {
				ok = 0;
				break;
			}
			key = key * 10 + def[i] - '0';
		}
		if (!ok || !udk_valid(key)) {
			free(def);
			continue;
		}
		slash++;
		n = (int)strlen(slash);
		if (n == 0) {
			d->has_udk[key] = 0;
			buf_clear(&d->udk[key]);
			free(def);
			continue;
		}
		if (n % 2 || n / 2 > 256)
			ok = 0;
		for (i = 0; ok && i < n; i++)
			if (hexv(slash[i]) < 0)
				ok = 0;
		if (ok) {
			buf_clear(&d->udk[key]);
			for (i = 0; i < n; i += 2)
				buf_addc(&d->udk[key],
					 (char)(hexv(slash[i]) * 16 + hexv(slash[i + 1])));
			d->has_udk[key] = 1;
		}
		free(def);
	}
	d->udk_locked = lock == 0;
}

static int udk_number(int k)
{
	switch (k) {
	case DMC_KEY_F6: return 17;
	case DMC_KEY_F7: return 18;
	case DMC_KEY_F8: return 19;
	case DMC_KEY_F9: return 20;
	case DMC_KEY_F10: return 21;
	case DMC_KEY_F11: return 23;
	case DMC_KEY_F12: return 24;
	case DMC_KEY_F13: return 25;
	case DMC_KEY_F14: return 26;
	case DMC_KEY_F15: return 28;
	case DMC_KEY_F16: return 29;
	case DMC_KEY_F17: return 31;
	case DMC_KEY_F18: return 32;
	case DMC_KEY_F19: return 33;
	case DMC_KEY_F20: return 34;
	}
	return 0;
}

static void udk_reset(struct dec *d)
{
	int i;
	for (i = 0; i < NUDK; i++) {
		d->has_udk[i] = 0;
		buf_clear(&d->udk[i]);
	}
	d->udk_locked = 0;
}

/* ------------------------------------------------------------------ */
/* the sub-terminals: VT52 mode, Tek mode */

static void sub_exit(void *ctx)
{
	struct dec *d = (struct dec *)ctx;
	d->sub_done = 1;
}

static void end_sub(struct dec *d)
{
	struct vt_interp *s = d->sub;
	if (!s)
		return;
	d->sub = NULL;
	d->sub_done = 0;
	if (d->sub_tek)
		vt_set_alt_screen(T(d), 0, 0);
	else
		T(d)->vt52_keys = 0;
	d->sub_tek = 0;
	interp_free(s);
}

static void enter_vt52(struct dec *d)
{
	end_sub(d);
	d->sub = vt52_new(T(d), G_DEC_GRAPHICS, "\x1b/Z", sub_exit, d);
	d->sub_tek = 0;
}

static void enter_tek(struct dec *d)
{
	end_sub(d);
	vt_set_alt_screen(T(d), 1, 1);
	d->sub = tek_new(T(d), sub_exit, d);
	d->sub_tek = 1;
}

/* ------------------------------------------------------------------ */
/* the parser */

static void control(struct dec *d, int32_t cp);

static void print_char(struct dec *d, int32_t cp)
{
	dmc_vt *t = T(d);
	uint32_t c = sets_map(d, (uint32_t)cp, t->charset == CS_LATIN1);
	if (d->status_active)
		status_print(t, (int32_t)c, t->style);
	else
		vt_print(t, (int32_t)c);
}

static void start_csi(struct dec *d)
{
	d->state = S_CSI;
	d->nparams = 0;
	d->marker = 0;
	d->inter = 0;
	memset(d->params, 0, sizeof(d->params));
	memset(d->colon, 0, sizeof(d->colon));
}

static void start_osc(struct dec *d)
{
	buf_clear(&d->osc);
	d->osc_len = 0;
	d->state = S_OSC;
}

static void start_dcs(struct dec *d)
{
	start_csi(d);
	d->state = S_DCS;
}

static void ground(struct dec *d, int32_t cp)
{
	dmc_vt *t = T(d);
	if (cp < 0x20 || cp == 0x7f) {
		control(d, cp);
		return;
	}
	if (cp >= 0x80 && cp <= 0x9f) {
		/* C1 controls (8 bit): as their ESC forms */
		switch (cp) {
		case 0x9b: start_csi(d); break;
		case 0x9d: start_osc(d); break;
		case 0x90: start_dcs(d); break;
		case 0x98: case 0x9e: case 0x9f: d->state = S_STRING; break;
		case 0x84: vt_index(t); break;
		case 0x8e: if (d->level >= 2) d->single = 2; break;
		case 0x8f: if (d->level >= 2) d->single = 3; break;
		case 0x85: vt_carriage_return(t); vt_index(t); break;
		case 0x8d: vt_reverse_index(t); break;
		case 0x88: vt_set_tab(t); break;
		}
		return;
	}
	print_char(d, cp);
}

static void control(struct dec *d, int32_t cp)
{
	dmc_vt *t = T(d);
	if (d->status_active && (cp == 0x0d || cp == 0x08)) {
		/* (on the status line only CR and BS move the cursor) */
		status_move(t, cp == 0x0d ? 0 : t->status.x - 1);
		return;
	}
	switch (cp) {
	case 0x05:
		if (t->answerback[0])
			vt_reply(t, t->answerback);
		break;
	case 0x07: vt_bell(t); break;
	case 0x08: vt_backspace(t); break;
	case 0x09: vt_tab(t, 1); break;
	case 0x0a: case 0x0b: case 0x0c:
		if (d->printer.autop)
			printer_line(&d->printer, t->cy);
		vt_index(t);
		if (t->newline_mode)
			vt_carriage_return(t);
		break;
	case 0x0d: vt_carriage_return(t); break;
	case 0x0e: d->gl = 1; break;
	case 0x0f: d->gl = 0; break;
	case 0x18: case 0x1a: d->state = S_GROUND; break;
	case 0x1b: d->state = S_ESC; break;
	}
}

/* whether this terminal (at the level it works at) has set g */
static int knows(const struct dec *d, int g)
{
	return XTERM(d) || gset_level(g) <= d->level;
}

static void designate(struct dec *d, int slot, const char *des, int is96)
{
	int g;
	if (slot >= 2 && d->level < 2 && !XTERM(d))
		return;
	if (soft_names(&d->soft, des, is96))
		g = G_SOFT;
	else if (!strcmp(des, "<") && !is96)
		g = d->preferred;
	else if ((g = gset_by_designator(des, is96)) < 0)
		return;
	if (g == G_SOFT || knows(d, g))
		d->g[slot] = (uint8_t)g;
}

static void esc_inter(struct dec *d, int32_t cp)
{
	dmc_vt *t = T(d);
	int i = d->inter;
	char des[4];
	const char *slots94 = "()*+", *slots96 = "-./";
	if (cp >= 0x20 && cp <= 0x2f && strchr("()*+-./", i) &&
	    !d->designation[0]) {
		/* a designator of two characters (DEC Supplemental's % 5) */
		d->designation[0] = (char)cp;
		d->designation[1] = 0;
		return;
	}
	d->state = S_GROUND;
	if (cp < 0x20 || cp > 0x7e)
		return;
	strcpy(des, d->designation);
	des[strlen(des) + 1] = 0;
	des[strlen(des)] = (char)cp;
	if (strchr(slots94, i))
		designate(d, (int)(strchr(slots94, i) - slots94), des, 0);
	else if (strchr(slots96, i))
		designate(d, (int)(strchr(slots96, i) - slots96) + 1, des, 1);
	else if (i == ' ') {
		if (cp == 'F')
			t->eight_bit_replies = 0;
		else if (cp == 'G' && d->level >= 2)
			t->eight_bit_replies = 1;
	} else if (i == '#') {
		switch (cp) {
		case '3': vt_set_line_size(t, LS_DOUBLE_TOP); break;
		case '4': vt_set_line_size(t, LS_DOUBLE_BOTTOM); break;
		case '5': vt_set_line_size(t, LS_SINGLE); break;
		case '6': vt_set_line_size(t, LS_DOUBLE_WIDTH); break;
		case '8': vt_alignment_test(t); break;
		}
	}
}

/* DECBI */
static void back_index(dmc_vt *t)
{
	if (t->cx > t->left)
		vt_move_cursor(t, t->cx - 1, t->cy, 0);
	else if (t->cx == t->left && t->cy >= t->top && t->cy <= t->bottom)
		insert_columns(t, t->left, 1);
}

/* DECFI */
static void forward_index(dmc_vt *t)
{
	if (t->cx < t->right)
		vt_move_cursor(t, t->cx + 1, t->cy, 0);
	else if (t->cx == t->right && t->cy >= t->top && t->cy <= t->bottom)
		delete_columns(t, t->left, 1);
}

static void esc(struct dec *d, int32_t cp)
{
	dmc_vt *t = T(d);
	d->state = S_GROUND;
	if (d->vt420 && d->level >= 4) {
		if (cp == '6') {
			back_index(t);
			return;
		}
		if (cp == '9') {
			forward_index(t);
			return;
		}
	}
	switch (cp) {
	case '[': start_csi(d); break;
	case ']': start_osc(d); break;
	case 'P': start_dcs(d); break;
	case 'X': case '^': case '_': d->state = S_STRING; break;
	case '(': case ')': case '*': case '+': case '-': case '.': case '/':
	case '#': case ' ': case '%':
		d->inter = cp;
		d->designation[0] = 0;
		d->state = S_ESC_INTER;
		break;
	case '7': vt_save_cursor(t); break;
	case '8': vt_restore_cursor(t); break;
	case 'D': vt_index(t); break;
	case 'E': vt_carriage_return(t); vt_index(t); break;
	case 'M': vt_reverse_index(t); break;
	case 'H': vt_set_tab(t); break;
	case 'c': vt_full_reset(t); break;
	case 'Z': vt_reply(t, d->p->da1); break;
	case 'n': if (d->level >= 2) d->gl = 2; break;
	case 'o': if (d->level >= 2) d->gl = 3; break;
	case '~': if (d->level >= 2) d->gr = 1; break;
	case '}': if (d->level >= 2) d->gr = 2; break;
	case '|': if (d->level >= 2) d->gr = 3; break;
	case 'N': if (d->level >= 2) d->single = 2; break;
	case 'O': if (d->level >= 2) d->single = 3; break;
	case '=': t->app_keypad = 1; break;
	case '>': t->app_keypad = 0; break;
	case '\\': break;
	default:
		if (cp < 0x20)
			control(d, cp);
	}
}

static void dispatch_csi(struct dec *d, int final);

static void csi(struct dec *d, int32_t cp)
{
	if (cp >= '0' && cp <= '9') {
		if (d->nparams == 0)
			d->nparams = 1;
		if (d->nparams <= NPARAMS) {
			int i = d->nparams - 1;
			d->params[i] = d->params[i] * 10 + (cp - '0');
			if (d->params[i] > 99999)
				d->params[i] = 99999;
		}
	} else if (cp == ';' || cp == ':') {
		if (d->nparams == 0)
			d->nparams = 1;
		if (d->nparams < NPARAMS) {
			d->colon[d->nparams] = cp == ':';
			d->nparams++;
		}
	} else if (cp >= 0x3c && cp <= 0x3f) {
		if (d->nparams == 0 && d->inter == 0)
			d->marker = cp;
	} else if (cp >= 0x20 && cp <= 0x2f) {
		d->inter = cp;
	} else if (cp >= 0x40 && cp <= 0x7e) {
		d->state = S_GROUND;
		dispatch_csi(d, cp);
	} else if (cp == 0x1b) {
		d->state = S_ESC;
	} else if (cp == 0x18 || cp == 0x1a) {
		d->state = S_GROUND;
	} else if (cp < 0x20) {
		control(d, cp);
	}
}

/* ------------------------------------------------------------------ */
/* the modes */

static int b12(int v)
{
	return v ? 1 : 2;
}

static void vt420_mode(struct dec *d, int m, int on)
{
	switch (m) {
	case 66: T(d)->app_keypad = on; break;
	case 67: d->backarrow_bs = on; break;
	case 69: vt_set_lr_margins(T(d), on); break;
	}
}

static void dec_mode(struct dec *d, int m, int on)
{
	dmc_vt *t = T(d);
	switch (m) {
	case 1: t->app_cursor = on; break;
	case 2: if (!on) enter_vt52(d); break;
	case 38: if (on && HAS(d, F_TEK)) enter_tek(d); break;
	case 3: if (!XTERM(d) || d->allow_columns) vt_set_column_mode(t, on); break;
	case 5: t->reverse_video = on; t->changed = 1; break;
	case 6: t->origin_mode = on; vt_cursor_position(t, 0, 0); break;
	case 7: t->autowrap = on; break;
	case 25: t->cursor_visible = on; break;
	case 80: if (HAS(d, F_SIXEL)) t->sixel_scrolling = !on; break;
	}
	if (!XTERM(d))
		return;
	switch (m) {
	case 47: case 1047: vt_set_alt_screen(t, on, m == 1047); break;
	case 1048: if (on) vt_save_cursor(t); else vt_restore_cursor(t); break;
	case 1049:
		if (on) {
			vt_save_cursor(t);
			vt_set_alt_screen(t, 1, 1);
		} else {
			vt_set_alt_screen(t, 0, 0);
			vt_restore_cursor(t);
		}
		break;
	case 2004: t->bracketed_paste = on; break;
	case 40: d->allow_columns = on; break;
	case 9: t->mouse_mode = on ? MOUSE_X10 : MOUSE_NONE; break;
	case 1000: t->mouse_mode = on ? MOUSE_NORMAL : MOUSE_NONE; break;
	case 1002: t->mouse_mode = on ? MOUSE_BUTTON : MOUSE_NONE; break;
	case 1003: t->mouse_mode = on ? MOUSE_ANY : MOUSE_NONE; break;
	case 1004: t->focus_events = on; break;
	case 1005: t->mouse_utf8 = on; break;
	case 1006: t->mouse_sgr = on; break;
	case 1015: t->mouse_urxvt = on; break;
	case 8452: t->sixel_cursor_right = on; break;
	}
}

static int dec_mode_state(struct dec *d, int m)
{
	dmc_vt *t = T(d);
	int r = 0;
	if (d->vt420 && d->level >= 4) {
		switch (m) {
		case 66: return b12(t->app_keypad);
		case 67: return b12(d->backarrow_bs);
		case 69: return b12(t->lr_margins);
		}
	}
	switch (m) {
	case 1: r = b12(t->app_cursor); break;
	case 2: r = 1; break;
	case 3: r = b12(t->requested_cols == 132); break;
	case 5: r = b12(t->reverse_video); break;
	case 6: r = b12(t->origin_mode); break;
	case 7: r = b12(t->autowrap); break;
	case 25: r = b12(t->cursor_visible); break;
	case 80: r = HAS(d, F_SIXEL) ? b12(!t->sixel_scrolling) : 0; break;
	}
	if (r || !XTERM(d))
		return r;
	switch (m) {
	case 47: case 1047: case 1049: return b12(vt_alt_screen(t));
	case 2004: return b12(t->bracketed_paste);
	case 9: return b12(t->mouse_mode == MOUSE_X10);
	case 1000: return b12(t->mouse_mode == MOUSE_NORMAL);
	case 1002: return b12(t->mouse_mode == MOUSE_BUTTON);
	case 1003: return b12(t->mouse_mode == MOUSE_ANY);
	case 40: return b12(d->allow_columns);
	case 1004: return b12(t->focus_events);
	case 1005: return b12(t->mouse_utf8);
	case 1006: return b12(t->mouse_sgr);
	case 1015: return b12(t->mouse_urxvt);
	case 8452: return b12(t->sixel_cursor_right);
	}
	return 0;
}

static int ansi_mode_state(struct dec *d, int m)
{
	if (m == 4)
		return b12(T(d)->insert_mode);
	if (m == 20)
		return b12(T(d)->newline_mode);
	return 0;
}

static void ansi_mode(struct dec *d, int m, int on)
{
	if (m == 4)
		T(d)->insert_mode = on;
	else if (m == 20)
		T(d)->newline_mode = on;
}

static void window_report(struct dec *d, int op)
{
	dmc_vt *t = T(d);
	switch (op) {
	case 14: vt_replyf(t, "\x1b[4;%d;%dt", t->rows * t->cell_ph, t->cols * t->cell_pw); break;
	case 16: vt_replyf(t, "\x1b[6;%d;%dt", t->cell_ph, t->cell_pw); break;
	case 18: vt_replyf(t, "\x1b[8;%d;%dt", t->rows, t->cols); break;
	case 19: vt_replyf(t, "\x1b[9;%d;%dt", t->rows, t->cols); break;
	}
}

static int mini(int a, int b)
{
	return a < b ? a : b;
}

static void graphics_attributes(struct dec *d, int item, int action)
{
	dmc_vt *t = T(d);
	switch (item) {
	case 1:
		vt_reply(t, "\x1b[?1;0;256S");
		break;
	case 2:
		if (action == 1 || action == 4)
			vt_replyf(t, "\x1b[?2;0;%d;%dS",
				  mini(t->cols * t->cell_pw, VT_MAX_IMAGE),
				  mini(t->rows * t->cell_ph, VT_MAX_IMAGE));
		else
			vt_reply(t, "\x1b[?2;3;0S");
		break;
	default:
		vt_replyf(t, "\x1b[?%d;1;0S", item);
	}
}

/* ------------------------------------------------------------------ */
/* SGR */

static void sgr(struct dec *d)
{
	dmc_vt *t = T(d);
	int kept = st_attrs(t->style) & A_PROTECTED;
	int color = HAS(d, F_COLOR), i = 0, n = d->nparams;
	vt_style s = t->style;
	if (n == 0) {
		t->style = st_with_attrs(ST_NORMAL, kept);
		return;
	}
	while (i < n) {
		int v = d->params[i], a = st_attrs(s);
		switch (v) {
		case 0: s = ST_NORMAL; break;
		case 1: a |= A_BOLD; break;
		case 2: a |= A_FAINT; break;
		case 3: a |= A_ITALIC; break;
		case 4: {
			int sub = 1;
			a &= ~A_ANY_UNDER;
			if (i + 1 < n && d->colon[i + 1])
				sub = d->params[++i];
			a |= sub == 0 ? 0 : sub == 2 ? A_DUNDER :
				sub >= 3 && sub <= 5 ? A_CURLY : A_UNDERLINE;
			break;
		}
		case 21: a = (a & ~A_ANY_UNDER) | A_DUNDER; break;
		case 5: case 6: a |= A_BLINK; break;
		case 7: a |= A_INVERSE; break;
		case 8: a |= A_INVISIBLE; break;
		case 9: a |= A_STRIKE; break;
		case 22: a &= ~(A_BOLD | A_FAINT); break;
		case 23: a &= ~A_ITALIC; break;
		case 24: a &= ~A_ANY_UNDER; break;
		case 53: a |= A_OVERLINE; break;
		case 55: a &= ~A_OVERLINE; break;
		case 25: a &= ~A_BLINK; break;
		case 27: a &= ~A_INVERSE; break;
		case 28: a &= ~A_INVISIBLE; break;
		case 29: a &= ~A_STRIKE; break;
		case 39: s = st_with_fg(s, ST_DEFAULT_FG); break;
		case 49: s = st_with_bg(s, ST_DEFAULT_BG); break;
		case 58:
			/* the underline's colour: read past */
			if (i + 1 < n) {
				if (d->params[i + 1] == 5)
					i += 2;
				else if (d->params[i + 1] == 2)
					i += d->colon[i + 1] && i + 5 < n && d->colon[i + 5] ? 5 : 4;
				else
					i = n;
			}
			break;
		case 38: case 48: {
			int c = -1;
			if (i + 1 < n) {
				if (d->params[i + 1] == 5) {
					if (i + 2 < n) {
						c = d->params[i + 2] & 0xff;
						i += 2;
					}
				} else if (d->params[i + 1] == 2) {
					int j = i + 2;
					if (d->colon[i + 1] && i + 5 < n && d->colon[i + 5])
						j++;
					if (j + 2 < n) {
						c = ST_RGB | ((d->params[j] & 0xff) << 16) |
							((d->params[j + 1] & 0xff) << 8) |
							(d->params[j + 2] & 0xff);
						i = j + 2;
					} else {
						i = n;
					}
				} else {
					i = n;
				}
			}
			if (c >= 0 && color)
				s = v == 38 ? st_with_fg(s, c) : st_with_bg(s, c);
			break;
		}
		default:
			if (v >= 30 && v <= 37) {
				if (color)
					s = st_with_fg(s, v - 30);
			} else if (v >= 40 && v <= 47) {
				if (color)
					s = st_with_bg(s, v - 40);
			} else if (v >= 90 && v <= 97) {
				if (color)
					s = st_with_fg(s, v - 90 + 8);
			} else if (v >= 100 && v <= 107) {
				if (color)
					s = st_with_bg(s, v - 100 + 8);
			}
		}
		if (v != 0 && v != 38 && v != 48 && v != 58 &&
		    !(v >= 30 && v <= 49) && !(v >= 90 && v <= 107))
			s = st_with_attrs(s, a);
		i++;
	}
	t->style = st_with_attrs(s, st_attrs(s) | kept);
}

static void sgr_color(dmc_buf *b, int c, int base, int bright)
{
	char s[48];
	if (c == ST_DEFAULT_FG || c == ST_DEFAULT_BG)
		return;
	if (c & ST_RGB)
		snprintf(s, sizeof(s), ";%d;2;%d;%d;%d", base + 8, (c >> 16) & 0xff,
			 (c >> 8) & 0xff, c & 0xff);
	else if (c < 8)
		snprintf(s, sizeof(s), ";%d", base + c);
	else if (c < 16)
		snprintf(s, sizeof(s), ";%d", bright + c - 8);
	else
		snprintf(s, sizeof(s), ";%d;5;%d", base + 8, c);
	buf_adds(b, s);
}

static void sgr_string(struct dec *d, dmc_buf *b)
{
	vt_style st = T(d)->style;
	int a = st_attrs(st);
	buf_adds(b, "0");
	if (a & A_BOLD) buf_adds(b, ";1");
	if (a & A_FAINT) buf_adds(b, ";2");
	if (a & A_ITALIC) buf_adds(b, ";3");
	if (a & A_UNDERLINE) buf_adds(b, ";4");
	if (a & A_DUNDER) buf_adds(b, ";21");
	if (a & A_CURLY) buf_adds(b, ";4:3");
	if (a & A_BLINK) buf_adds(b, ";5");
	if (a & A_INVERSE) buf_adds(b, ";7");
	if (a & A_INVISIBLE) buf_adds(b, ";8");
	if (a & A_STRIKE) buf_adds(b, ";9");
	if (a & A_OVERLINE) buf_adds(b, ";53");
	sgr_color(b, st_fg(st), 30, 90);
	sgr_color(b, st_bg(st), 40, 100);
}

/* ------------------------------------------------------------------ */
/* OSC */

/* "rgb:r/g/b" (1-4 hex digits each) or "#rgb" (1-4 each): ARGB, or 0 */
static uint32_t parse_color(const char *spec)
{
	char s[64];
	const char *parts[3];
	int lens[3], i, n;
	uint32_t v[3];
	while (*spec == ' ')
		spec++;
	n = (int)strlen(spec);
	while (n > 0 && spec[n - 1] == ' ')
		n--;
	if (n <= 0 || n >= (int)sizeof(s))
		return 0;
	memcpy(s, spec, (size_t)n);
	s[n] = 0;
	if (!strncmp(s, "rgb:", 4)) {
		char *a = s + 4, *b = strchr(a, '/'), *c;
		if (!b)
			return 0;
		c = strchr(b + 1, '/');
		if (!c || strchr(c + 1, '/'))
			return 0;
		parts[0] = a;
		lens[0] = (int)(b - a);
		parts[1] = b + 1;
		lens[1] = (int)(c - b - 1);
		parts[2] = c + 1;
		lens[2] = (int)strlen(c + 1);
	} else if (s[0] == '#' && n > 1 && (n - 1) % 3 == 0) {
		int k = (n - 1) / 3;
		for (i = 0; i < 3; i++) {
			parts[i] = s + 1 + i * k;
			lens[i] = k;
		}
	} else {
		return 0;
	}
	for (i = 0; i < 3; i++) {
		uint32_t x = 0;
		int j;
		if (lens[i] < 1 || lens[i] > 4)
			return 0;
		for (j = 0; j < lens[i]; j++) {
			int h = hexv(parts[i][j]);
			if (h < 0)
				return 0;
			x = x * 16 + (uint32_t)h;
		}
		switch (lens[i]) {
		case 1: x *= 17; break;
		case 3: x >>= 4; break;
		case 4: x >>= 8; break;
		}
		v[i] = x;
	}
	return 0xff000000u | (v[0] << 16) | (v[1] << 8) | v[2];
}

static void color_spec(struct dec *d, int i, const char *spec,
		       const char *prefix)
{
	dmc_vt *t = T(d);
	uint32_t c;
	if (!strcmp(spec, "?")) {
		c = vt_shown_color(t, i);
		vt_replyf(t, "\x1b]%srgb:%04x/%04x/%04x%s", prefix,
			  ((c >> 16) & 0xff) * 0x101, ((c >> 8) & 0xff) * 0x101,
			  (c & 0xff) * 0x101, d->osc_bel ? "\a" : "\x1b\\");
		return;
	}
	c = parse_color(spec);
	if (c)
		vt_set_color_override(t, i, c);
}

/* the i'th field of s split at ';' into out (0 if there's none) */
static int field(const char *s, int i, char *out, int max)
{
	const char *e;
	while (i-- > 0) {
		s = strchr(s, ';');
		if (!s)
			return 0;
		s++;
	}
	e = strchr(s, ';');
	if (!e)
		e = s + strlen(s);
	if (e - s >= max)
		return 0;
	memcpy(out, s, (size_t)(e - s));
	out[e - s] = 0;
	return 1;
}

static int to_int(const char *s, int *v)
{
	long n = 0;
	if (!*s)
		return 0;
	for (; *s; s++) {
		if (!is_digit(*s) || n > 100000000L)
			return 0;
		n = n * 10 + (*s - '0');
	}
	*v = (int)n;
	return 1;
}

static void osc(struct dec *d)
{
	dmc_vt *t = T(d);
	const char *s = buf_str(&d->osc), *semi, *arg;
	char num[16], f[256], g[256], pre[16];
	int cmd, i, n;
	if (!XTERM(d))
		return;
	semi = strchr(s, ';');
	n = semi ? (int)(semi - s) : (int)strlen(s);
	if (n >= (int)sizeof(num))
		return;
	memcpy(num, s, (size_t)n);
	num[n] = 0;
	if (!to_int(num, &cmd))
		return;
	arg = semi ? semi + 1 : "";
	switch (cmd) {
	case 0: case 2:
		vt_set_title(t, arg);
		break;
	case 4:
		for (i = 0; field(arg, i, f, sizeof(f)) && field(arg, i + 1, g, sizeof(g)); i += 2)
			if (to_int(f, &n) && n >= 0 && n <= 255) {
				snprintf(pre, sizeof(pre), "4;%d;", n);
				color_spec(d, n, g, pre);
			}
		break;
	case 10: case 11: case 12:
		n = cmd;
		for (i = 0; n <= 12 && field(arg, i, f, sizeof(f)); i++, n++) {
			snprintf(pre, sizeof(pre), "%d;", n);
			color_spec(d, 256 + n - 10, f, pre);
		}
		break;
	case 104:
		if (!*arg)
			vt_reset_colors(t, 0, 255);
		else
			for (i = 0; field(arg, i, f, sizeof(f)); i++)
				if (to_int(f, &n) && n >= 0 && n <= 255)
					vt_reset_colors(t, n, n);
		break;
	case 110: vt_reset_colors(t, 256, 256); break;
	case 111: vt_reset_colors(t, 257, 257); break;
	case 112: vt_reset_colors(t, 258, 258); break;
	case 8: {
		/* a hyperlink: params;uri, an empty uri ends it */
		const char *uri = strchr(arg, ';');
		link_unref(t->link);
		t->link = NULL;
		if (uri && uri[1] && strlen(uri + 1) <= 2048)
			t->link = link_new(uri + 1);
		break;
	}
	}
}

/* ------------------------------------------------------------------ */
/* the status line, reports */

static int status_csi(struct dec *d, int c)
{
	dmc_vt *t = T(d);
	if (d->marker || d->inter)
		return !d->marker && d->inter == '$' && c != '}' && c != '~';
	switch (c) {
	case 'C': status_move(t, t->status.x + p(d, 0, 1)); break;
	case 'D': status_move(t, t->status.x - p(d, 0, 1)); break;
	case 'G': case '`': status_move(t, p(d, 0, 1) - 1); break;
	case 'H': case 'f': status_move(t, p(d, 1, 1) - 1); break;
	case 'K': status_erase(t, p(d, 0, 0), st_erased(t->style)); break;
	case 'm': case 'h': case 'l': return 0;
	}
	return 1;
}

static int cursor_row(const dmc_vt *t)
{
	return t->origin_mode ? t->cy - t->top : t->cy;
}

static int cursor_col(const dmc_vt *t)
{
	return mini(t->cx, t->cols - 1);
}

static void dec_status(struct dec *d, int n)
{
	dmc_vt *t = T(d);
	if (n == 6) {
		vt_replyf(t, "\x1b[?%d;%d;%dR", cursor_row(t) + 1, cursor_col(t) + 1,
			  t->page + 1);
		return;
	}
	if (d->level < 2)
		return;
	switch (n) {
	case 15: vt_reply(t, HAS(d, F_PRINTER) ? "\x1b[?10n" : "\x1b[?13n"); break;
	case 25: vt_reply(t, d->udk_locked ? "\x1b[?21n" : "\x1b[?20n"); break;
	case 26: vt_reply(t, "\x1b[?27;1;0;0n"); break;
	}
}

static char bits4(int a, int b, int c, int e)
{
	return (char)(0x40 + (a ? 1 : 0) + (b ? 2 : 0) + (c ? 4 : 0) + (e ? 8 : 0));
}

static void presentation_state(struct dec *d, int n)
{
	dmc_vt *t = T(d);
	dmc_buf b;
	char s[96];
	int a = st_attrs(t->style), i;
	memset(&b, 0, sizeof(b));
	if (n == 1) {
		snprintf(s, sizeof(s), "\x1bP1$u%d;%d;1;%c;%c;%c;%d;%d;%c;",
			 cursor_row(t) + 1, cursor_col(t) + 1,
			 bits4(a & A_BOLD, a & A_ANY_UNDER, a & A_BLINK, a & A_INVERSE),
			 bits4(a & A_PROTECTED, 0, 0, 0),
			 bits4(t->origin_mode, d->single == 2, d->single == 3,
			       t->pending_wrap),
			 d->gl, d->gr,
			 bits4(set_is96(d, 0), set_is96(d, 1), set_is96(d, 2),
			       set_is96(d, 3)));
		buf_adds(&b, s);
		for (i = 0; i < 4; i++)
			buf_adds(&b, set_designator(d, i));
		buf_adds(&b, "\x1b\\");
		vt_reply(t, buf_str(&b));
	} else if (n == 2) {
		buf_adds(&b, "\x1bP2$u");
		vt_tab_stops(t, &b);
		buf_adds(&b, "\x1b\\");
		vt_reply(t, buf_str(&b));
	}
	buf_free(&b);
}

static void preferred_string(struct dec *d)
{
	vt_replyf(T(d), "\x1bP%d!u%s\x1b\\", gset_is96(d->preferred) ? 1 : 0,
		  gset_designator(d->preferred));
}

static void assign_preferred(struct dec *d, int size, const char *des)
{
	int g = gset_by_designator(des, size == 1);
	if (g == G_DEC_SUPPLEMENTAL || g == G_LATIN1)
		d->preferred = g;
}

static void conformance(struct dec *d, int level, int c1)
{
	if (level < 61 || level > 65)
		return;
	d->level = mini(level - 60, d->p->level);
	T(d)->eight_bit_replies = d->level >= 2 && c1 != 1;
	vt_soft_reset(T(d));
}

/* ------------------------------------------------------------------ */
/* a VT420's and VT5xx's CSIs */

/* the area from parameter at (Pt; Pl; Pb; Pr), cut to the screen */
static int area(struct dec *d, int at, int stream, struct area *a)
{
	dmc_vt *t = T(d);
	int oy = t->origin_mode ? t->top : 0, ox = t->origin_mode ? t->left : 0;
	int maxy = t->origin_mode ? t->bottom : t->rows - 1;
	int maxx = t->origin_mode ? t->right : t->cols - 1;
	a->top = clampi(p(d, at, 1) - 1 + oy, 0, maxy);
	a->left = clampi(p(d, at + 1, 1) - 1 + ox, 0, maxx);
	a->bottom = clampi(p(d, at + 2, maxy - oy + 1) - 1 + oy, 0, maxy);
	a->right = clampi(p(d, at + 3, maxx - ox + 1) - 1 + ox, 0, maxx);
	return !(a->top > a->bottom ||
		 (a->left > a->right && !(stream && a->top < a->bottom)));
}

static void page(struct dec *d, int n, int home)
{
	vt_select_page(T(d), clampi(n, 0, VT_PAGES - 1), home);
}

static int pct(uint32_t v)
{
	return (int)floor(v * 100 / 255.0 + 0.5);
}

static int level5(struct dec *d, int i, int c)
{
	dmc_vt *t = T(d);
	if (d->level < 5)
		return 0;
	if (i == '"' && c == 'v') {
		vt_replyf(t, "\x1b[%d;%d;1;1;%d\"w", t->rows, t->cols, t->page + 1);
	} else if (i == ' ' && (c == 't' || c == 'u' || c == '=' || c == '~')) {
		/* DECSWBV, DECSMBV, DECSKCV, DECTME: nothing to set */
	} else if (i == ',' && c == '|' && HAS(d, F_COLOR)) {
		if (param(d, 0) == 1) {
			vt_set_color_override(t, 256, vt_shown_color(t, clampi(param(d, 1), 0, 15)));
			vt_set_color_override(t, 257, vt_shown_color(t, clampi(param(d, 2), 0, 15)));
		}
	} else if (i == ',' && c == '}') {
		/* DECATC */
	} else if (i == '$' && c == 'u' && param(d, 0) == 2 && HAS(d, F_COLOR)) {
		dmc_buf b;
		char s[64];
		int n;
		memset(&b, 0, sizeof(b));
		buf_adds(&b, "\x1bP2$s");
		for (n = 0; n < 16; n++) {
			uint32_t col = vt_shown_color(t, n);
			snprintf(s, sizeof(s), "%s%d;2;%d;%d;%d", n ? "/" : "", n,
				 pct((col >> 16) & 0xff), pct((col >> 8) & 0xff),
				 pct(col & 0xff));
			buf_adds(&b, s);
		}
		buf_adds(&b, "\x1b\\");
		vt_reply(t, buf_str(&b));
		buf_free(&b);
	} else {
		return 0;
	}
	return 1;
}

static int vt420_csi(struct dec *d, int c)
{
	dmc_vt *t = T(d);
	int i = d->inter, k;
	struct area a;
	if (!d->vt420 || d->level < 4)
		return 0;
	if (d->marker == '?') {
		if (!i && (c == 'h' || c == 'l'))
			for (k = 0; k < (d->nparams > 1 ? d->nparams : 1); k++)
				vt420_mode(d, param(d, k), c == 'h');
		return 0;
	}
	if (d->marker)
		return 0;
	if (!i && c == 's' && t->lr_margins)
		vt_set_margins(t, p(d, 0, 1) - 1, p(d, 1, t->cols) - 1);
	else if (!i && c == 'U')
		page(d, t->page + p(d, 0, 1), 1);
	else if (!i && c == 'V')
		page(d, t->page - p(d, 0, 1), 1);
	else if (i == ' ' && c == 'P')
		page(d, p(d, 0, 1) - 1, 0);
	else if (i == ' ' && c == 'Q')
		page(d, t->page + p(d, 0, 1), 0);
	else if (i == ' ' && c == 'R')
		page(d, t->page - p(d, 0, 1), 0);
	else if (i == '\'' && c == '}') {
		if (vt_within_margins(t) && t->cy >= t->top && t->cy <= t->bottom)
			insert_columns(t, t->cx, p(d, 0, 1));
	} else if (i == '\'' && c == '~') {
		if (vt_within_margins(t) && t->cy >= t->top && t->cy <= t->bottom)
			delete_columns(t, t->cx, p(d, 0, 1));
	} else if (i == '$' && c == 'v') {
		if (area(d, 0, 0, &a)) {
			int oy = t->origin_mode ? t->top : 0;
			int ox = t->origin_mode ? t->left : 0;
			rect_copy(t, &a, p(d, 5, 1) - 1 + oy, p(d, 6, 1) - 1 + ox);
		}
	} else if (i == '$' && c == 'z') {
		if (area(d, 0, 0, &a))
			rect_erase(t, &a, 0);
	} else if (i == '$' && c == '{') {
		if (area(d, 0, 0, &a))
			rect_erase(t, &a, 1);
	} else if (i == '$' && c == 'x') {
		int ch = param(d, 0);
		if (area(d, 1, 0, &a) &&
		    ((ch >= 0x20 && ch <= 0x7e) || (ch >= 0xa0 && ch <= 0xff)))
			rect_fill(t, &a, ch);
	} else if (i == '$' && (c == 'r' || c == 't')) {
		if (area(d, 0, !d->rectangle, &a))
			rect_attributes(t, &a, d->params + 4,
					d->nparams > 4 ? d->nparams - 4 : 0,
					c == 't', d->rectangle);
	} else if (i == '*' && c == 'x') {
		d->rectangle = param(d, 0) == 2;
	} else if (i == '*' && c == 'y') {
		int sum = area(d, 2, 0, &a) ? rect_checksum(t, &a) : 0;
		vt_replyf(t, "\x1bP%d!~%04X\x1b\\", param(d, 0), sum);
	} else if (i == '*' && c == '|') {
		t->requested_rows = clampi(p(d, 0, 24), 1, 255);
	} else if (!i && c == 't' && param(d, 0) >= 24) {
		t->requested_rows = mini(param(d, 0), 255);
	} else if (i == '$' && c == '|') {
		t->requested_cols = p(d, 0, 80) > 80 ? 132 : 80;
	} else {
		return level5(d, i, c);
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* ANSI-BBS's CSIs (CTerm's) */

static const int speeds[] = {
	0, 300, 600, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 76800, 115200
};

/* CSI 0 ; n SP D: SyncTERM's font n for the text (the profile's own: back
   to it); one it hasn't got ignored */
static void select_font(struct dec *d, int n)
{
	dmc_vt *t = T(d);
	const struct vt_font *own = machine_font(d->p->font), *f;
	if (n == d->p->syncterm) {
		t->host_font = NULL;
		t->changed = 1;
		return;
	}
	f = syncterm_font(n, own ? own->h : 16, t->charset);
	if (f) {
		t->host_font = f;
		t->changed = 1;
	}
}

static int bbs_csi(struct dec *d, int c)
{
	dmc_vt *t = T(d);
	int m = d->marker, i = d->inter, k;
	if (!m && !i && (c == '|' || (c == 'N' && d->music_mode >= 1))) {
		d->in_music = 1;
		buf_clear(&d->music);
		return 1;
	}
	if (!m && !i && c == 'M' && d->music_mode == 2 && d->nparams == 0) {
		d->maybe_music = 1;
		return 1;
	}
	if (m == '=' && c == 'M') {
		d->music_mode = clampi(p(d, 0, 0), 0, 2);
		return 1;
	}
	if (m == '=' && (c == 'h' || c == 'l')) {
		if (param(d, 0) == 255)
			d->doorway = c == 'h';
		return 1;
	}
	if (m == '?' && (c == 'h' || c == 'l')) {
		for (k = 0; k < (d->nparams > 1 ? d->nparams : 1); k++)
			if (param(d, k) == 33) {
				t->ice_colors = c == 'h';
				t->changed = 1;
			}
		return 0;
	}
	if (!m && i == '*' && c == 'r') {
		if (param(d, 0) <= 1)
			t->emulated_bps = param(d, 1) >= 0 && param(d, 1) < 12 ?
				speeds[param(d, 1)] : 0;
		return 1;
	}
	if (!m && i == ' ' && c == 'D') {
		if (param(d, 0) <= 0)
			select_font(d, p(d, 1, 0));
		return 1;
	}
	if (d->rip && !m && !i && c == '!') {
		rip_query(d->rip);
		return 1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* the CSI dispatched */

static void dispatch_csi(struct dec *d, int final)
{
	dmc_vt *t = T(d);
	int c = final, i, n;
	if (d->bbs && bbs_csi(d, c))
		return;
	if (vt420_csi(d, c))
		return;
	if (d->status_active && status_csi(d, c))
		return;
	if (d->marker == '?') {
		if (d->inter == '$' && c == 'p') {
			vt_replyf(t, "\x1b[?%d;%d$y", p(d, 0, 0),
				  dec_mode_state(d, p(d, 0, 0)));
			return;
		}
		switch (c) {
		case 'h': case 'l':
			for (i = 0; i < (d->nparams > 1 ? d->nparams : 1); i++)
				dec_mode(d, d->params[i], c == 'h');
			break;
		case 'J': vt_erase_display(t, p(d, 0, 0), d->level >= 2); break;
		case 'K': vt_erase_line(t, p(d, 0, 0), d->level >= 2); break;
		case 'n': dec_status(d, p(d, 0, 0)); break;
		case 'S': if (XTERM(d)) graphics_attributes(d, p(d, 0, 0), p(d, 1, 0)); break;
		case 'i':
			if (HAS(d, F_PRINTER))
				switch (p(d, 0, 0)) {
				case 1: printer_line(&d->printer, t->cy); break;
				case 4: d->printer.autop = 0; break;
				case 5: d->printer.autop = 1; break;
				}
			break;
		}
		return;
	}
	if (d->marker == '>') {
		if (c == 'c' && d->p->da2)
			vt_reply(t, d->p->da2);
		else if (c == 'q' && XTERM(d))
			vt_reply(t, "\x1bP>|D-Modem 1.0\x1b\\");
		return;
	}
	if (d->marker == '=') {
		if (c == 'c' && (d->p->level >= 4 || XTERM(d)))
			vt_reply(t, "\x1bP!|00000000\x1b\\");
		return;
	}
	if (d->marker)
		return;
	if (d->inter) {
		int in = d->inter;
		if (in == '!' && c == 'p') {
			vt_soft_reset(t);
		} else if (in == ' ' && c == 'q') {
			t->cursor_style = clampi(p(d, 0, 0), 0, 6);
		} else if (in == '$' && c == 'p') {
			vt_replyf(t, "\x1b[%d;%d$y", p(d, 0, 0), ansi_mode_state(d, p(d, 0, 0)));
		} else if (in == '"' && c == 'q' && d->level >= 2) {
			int a = st_attrs(t->style) & ~A_PROTECTED;
			t->style = st_with_attrs(t->style, p(d, 0, 0) == 1 ? a | A_PROTECTED : a);
		} else if (in == '"' && c == 'p' && d->p->level >= 2) {
			conformance(d, p(d, 0, 0), param(d, 1));
		} else if (in == '$' && c == 'w' && (d->level >= 3 || XTERM(d))) {
			presentation_state(d, p(d, 0, 0));
		} else if (in == '$' && c == '~' && d->level >= 3) {
			t->status_type = clampi(p(d, 0, 0), 0, 2);
			if (t->status_type != 2)
				d->status_active = 0;
			t->changed = 1;
		} else if (in == '$' && c == '}' && d->level >= 3) {
			d->status_active = p(d, 0, 0) == 1 && t->status_type == 2;
		} else if (in == '&' && c == 'u' && (d->level >= 3 || XTERM(d))) {
			preferred_string(d);
		}
		return;
	}
	switch (c) {
	case 'A': vt_move_cursor(t, t->cx, t->cy - p(d, 0, 1), 1); break;
	case 'B': case 'e': vt_move_cursor(t, t->cx, t->cy + p(d, 0, 1), 1); break;
	case 'C': case 'a': vt_move_cursor(t, t->cx + p(d, 0, 1), t->cy, 0); break;
	case 'D': vt_move_cursor(t, t->cx - p(d, 0, 1), t->cy, 0); break;
	case 'E': vt_move_cursor(t, 0, t->cy + p(d, 0, 1), 1); break;
	case 'F': vt_move_cursor(t, 0, t->cy - p(d, 0, 1), 1); break;
	case 'G': case '`': vt_move_cursor(t, p(d, 0, 1) - 1, t->cy, 0); break;
	case 'H': case 'f': vt_cursor_position(t, p(d, 1, 1) - 1, p(d, 0, 1) - 1); break;
	case 'd': vt_cursor_position(t, t->cx, p(d, 0, 1) - 1); break;
	case 'I': vt_tab(t, p(d, 0, 1)); break;
	case 'Z': vt_back_tab(t, p(d, 0, 1)); break;
	case 'J': vt_erase_display(t, p(d, 0, 0), 0); break;
	case 'K': vt_erase_line(t, p(d, 0, 0), 0); break;
	case 'L': vt_insert_lines(t, p(d, 0, 1)); break;
	case 'M': vt_delete_lines(t, p(d, 0, 1)); break;
	case '@': vt_insert_chars(t, p(d, 0, 1)); break;
	case 'P': vt_delete_chars(t, p(d, 0, 1)); break;
	case 'X': vt_erase_chars(t, p(d, 0, 1)); break;
	case 'S':
		for (n = mini(p(d, 0, 1), t->rows); n > 0; n--)
			vt_scroll_up(t, t->top, t->bottom);
		break;
	case 'T':
		for (n = mini(p(d, 0, 1), t->rows); n > 0; n--)
			vt_scroll_down(t, t->top, t->bottom);
		break;
	case 'b':
		for (n = mini(p(d, 0, 1), t->cols * t->rows); n > 0; n--)
			vt_print(t, t->last_char);
		break;
	case 'g':
		if (p(d, 0, 0) == 0)
			vt_clear_tab(t);
		else if (p(d, 0, 0) == 3)
			vt_clear_tabs(t);
		break;
	case 'h': case 'l':
		for (i = 0; i < (d->nparams > 1 ? d->nparams : 1); i++)
			ansi_mode(d, d->params[i], c == 'h');
		break;
	case 'm': sgr(d); break;
	case 'n':
		if (p(d, 0, 0) == 5)
			vt_reply(t, "\x1b[0n");
		else if (p(d, 0, 0) == 6)
			vt_replyf(t, "\x1b[%d;%dR", cursor_row(t) + 1, cursor_col(t) + 1);
		break;
	case 'c': if (p(d, 0, 0) == 0) vt_reply(t, d->p->da1); break;
	case 'r': vt_set_region(t, p(d, 0, 1) - 1, p(d, 1, t->rows) - 1); break;
	case 's': vt_save_cursor(t); break;
	case 'u': vt_restore_cursor(t); break;
	case 't': if (XTERM(d)) window_report(d, p(d, 0, 0)); break;
	case 'x':
		if (p(d, 0, 0) <= 1 && d->nparams <= 1)
			vt_replyf(t, "\x1b[%d;1;1;128;128;1;0x",
				  (d->nparams == 0 ? 0 : d->params[0]) + 2);
		break;
	case 'q':
		for (i = 0; i < (d->nparams > 1 ? d->nparams : 1); i++) {
			int v = d->params[i];
			if (v == 0)
				t->leds = 0;
			else if (v >= 1 && v <= 4)
				t->leds |= 1 << (v - 1);
		}
		break;
	case 'i':
		if (HAS(d, F_PRINTER)) {
			if (p(d, 0, 0) == 0)
				printer_screen(&d->printer);
			else if (p(d, 0, 0) == 5)
				printer_start(&d->printer);
		}
		break;
	}
}

/* ------------------------------------------------------------------ */
/* DCS */

static void dcs_start(struct dec *d, int final)
{
	dmc_vt *t = T(d);
	int i = d->inter, m = d->marker, k;
	if (d->sixel) {
		sixel_free(d->sixel);
		d->sixel = NULL;
	}
	d->dcs_on = 0;
	d->in_regis = 0;
	d->dcs_kind[0] = 0;
	d->dcs_p0 = param(d, 0);
	d->dcs_p1 = param(d, 1);
	d->n_dcs_params = d->nparams > NPARAMS ? NPARAMS : d->nparams;
	for (k = 0; k < d->n_dcs_params; k++)
		d->dcs_params[k] = d->params[k];
	if (final == 'q' && !i && !m && HAS(d, F_SIXEL)) {
		/* P2 = 1: the pixels not drawn left transparent */
		d->sixel = sixel_new(p(d, 1, 0) == 1, d->registers,
				     mini(VT_MAX_IMAGE, t->cols * t->cell_pw * 2),
				     mini(VT_MAX_IMAGE, t->rows * t->cell_ph * 4));
	} else if (final == 'p' && !i && !m && d->registers) {
		d->in_regis = 1;
		if (!d->regis)
			d->regis = regis_new(t, d->registers);
		regis_begin(d->regis, p(d, 0, 0));
	} else if ((final == 'q' && i == '$' && (d->level >= 3 || XTERM(d))) ||
		   (final == 'q' && i == '+' && XTERM(d)) ||
		   (final == '|' && !i && d->level >= 2) ||
		   (final == '{' && !i && d->level >= 2) ||
		   (final == 'u' && i == '!' && (d->level >= 3 || XTERM(d))) ||
		   (final == 'p' && i == '$' && d->vt420 && d->level >= 5 &&
		    HAS(d, F_COLOR))) {
		d->dcs_on = 1;
		buf_clear(&d->dcs);
		d->dcs_len = 0;
		k = 0;
		if (i)
			d->dcs_kind[k++] = (char)i;
		d->dcs_kind[k++] = (char)final;
		d->dcs_kind[k] = 0;
	}
}

static void dcs_entry(struct dec *d, int32_t cp)
{
	if ((cp >= '0' && cp <= '9') || cp == ';' || cp == ':') {
		csi(d, cp);
		d->state = S_DCS;
	} else if (cp >= 0x3c && cp <= 0x3f) {
		if (d->nparams == 0 && d->inter == 0)
			d->marker = cp;
	} else if (cp >= 0x20 && cp <= 0x2f) {
		d->inter = cp;
	} else if (cp >= 0x40 && cp <= 0x7e) {
		d->state = S_DCS_PASS;
		dcs_start(d, cp);
	} else if (cp == 0x1b) {
		d->state = S_ESC;
	} else if (cp == 0x18 || cp == 0x1a) {
		d->state = S_GROUND;
	}
}

static void dcs_put(struct dec *d, int32_t cp)
{
	if (d->sixel) {
		sixel_feed(d->sixel, cp);
		return;
	}
	if (d->in_regis) {
		if (d->regis)
			regis_feed(d->regis, cp);
		return;
	}
	if (d->dcs_on && d->dcs_len < (!strcmp(d->dcs_kind, "{") ? 65536 : 1024)) {
		utf8_put(&d->dcs, (uint32_t)cp);
		d->dcs_len++;
	}
}

static void dcs_abort(struct dec *d)
{
	if (d->in_regis && d->regis)
		regis_abort(d->regis);
	d->in_regis = 0;
	if (d->sixel) {
		sixel_free(d->sixel);
		d->sixel = NULL;
	}
	d->dcs_on = 0;
}

/* DECRQSS */
static void status_string(struct dec *d, const char *q)
{
	dmc_vt *t = T(d);
	dmc_buf b;
	char s[64];
	int ok = 1;
	memset(&b, 0, sizeof(b));
	s[0] = 0;
	if (d->vt420 && d->level >= 4 && !strcmp(q, "s"))
		snprintf(s, sizeof(s), "%d;%ds", t->left + 1, t->right + 1);
	else if (d->vt420 && d->level >= 4 && !strcmp(q, "*x"))
		snprintf(s, sizeof(s), "%d*x", d->rectangle ? 2 : 1);
	else if (!strcmp(q, "m")) {
		sgr_string(d, &b);
		buf_addc(&b, 'm');
	} else if (!strcmp(q, "r"))
		snprintf(s, sizeof(s), "%d;%dr", t->top + 1, t->bottom + 1);
	else if (!strcmp(q, " q"))
		snprintf(s, sizeof(s), "%d q", t->cursor_style > 1 ? t->cursor_style : 1);
	else if (!strcmp(q, "\"q"))
		snprintf(s, sizeof(s), "%d\"q", st_attrs(t->style) & A_PROTECTED ? 1 : 0);
	else if (!strcmp(q, "\"p"))
		snprintf(s, sizeof(s), "%d;%d\"p", 60 + d->level, t->eight_bit_replies ? 0 : 1);
	else if (!strcmp(q, "t"))
		snprintf(s, sizeof(s), "%dt", t->rows);
	else if (!strcmp(q, "$|"))
		snprintf(s, sizeof(s), "%d$|", t->cols);
	else if (!strcmp(q, "*|"))
		snprintf(s, sizeof(s), "%d*|", t->rows);
	else if (!strcmp(q, "$~"))
		snprintf(s, sizeof(s), "%d$~", t->status_type);
	else if (!strcmp(q, "$}"))
		snprintf(s, sizeof(s), "%d$}", d->status_active ? 1 : 0);
	else
		ok = 0;
	if (ok) {
		dmc_buf r;
		memset(&r, 0, sizeof(r));
		buf_adds(&r, "\x1bP1$r");
		buf_adds(&r, s);
		buf_adds(&r, buf_str(&b));
		buf_adds(&r, "\x1b\\");
		vt_reply(t, buf_str(&r));
		buf_free(&r);
	} else {
		vt_reply(t, "\x1bP0$r\x1b\\");
	}
	buf_free(&b);
}

static uint32_t rgb_pct(int r, int g, int b)
{
#define C(v) ((uint32_t)floor(clampi((v), 0, 100) * 255 / 100.0 + 0.5))
	return 0xff000000u | (C(r) << 16) | (C(g) << 8) | C(b);
#undef C
}

/* DECRSTS 2: the colour table from DECCTR's entries */
static void restore_table(struct dec *d, const char *text)
{
	const char *s = text;
	while (*s) {
		const char *e = strchr(s, '/');
		int len = e ? (int)(e - s) : (int)strlen(s), f[5], k;
		char entry[128], v[32];
		s = e ? e + 1 : s + len;
		if (len >= (int)sizeof(entry))
			continue;
		memcpy(entry, s - len - (e ? 1 : 0), (size_t)len);
		entry[len] = 0;
		for (k = 0; k < 5; k++)
			if (!field(entry, k, v, sizeof(v)) || !to_int(v, &f[k]))
				f[k] = 0;
		if (!field(entry, 4, v, sizeof(v)) || f[0] < 0 || f[0] > 15)
			continue;
		if (f[1] == 1)
			vt_set_color_override(T(d), f[0], sixel_hls(f[2], f[3], f[4]));
		else if (f[1] == 2)
			vt_set_color_override(T(d), f[0], rgb_pct(f[2], f[3], f[4]));
	}
}

static void dcs_end(struct dec *d)
{
	dmc_vt *t = T(d);
	if (d->in_regis) {
		d->in_regis = 0;
		if (d->regis)
			regis_end(d->regis);
	}
	if (d->sixel) {
		struct sixel *sx = d->sixel;
		struct vt_image *img;
		d->sixel = NULL;
		img = sixel_finish(t, sx, t->cell_pw, t->cell_ph);
		sixel_free(sx);
		if (img)
			vt_place_image(t, img);
	}
	if (d->dcs_on) {
		const char *q = buf_str(&d->dcs), *k = d->dcs_kind;
		d->dcs_on = 0;
		if (!strcmp(k, "$q"))
			status_string(d, q);
		else if (!strcmp(k, "|"))
			udk_load(d, d->dcs_p0, d->dcs_p1, q);
		else if (!strcmp(k, "{"))
			soft_load(d, d->dcs_params, d->n_dcs_params, q);
		else if (!strcmp(k, "!u"))
			assign_preferred(d, d->dcs_p0, q);
		else if (!strcmp(k, "+q"))
			vt_reply(t, "\x1bP0+r\x1b\\");
		else if (!strcmp(k, "$p") && d->dcs_p0 == 2)
			restore_table(d, q);
	}
}

/* ------------------------------------------------------------------ */
/* ANSI-BBS's music, Avatar's codes: taken before the parser */

static int music_start(int c)
{
	return c > 0 && c < 0x80 && strchr("ABCDEFGLMNOPT<>", c) != NULL;
}

static int bbs_intercept(struct dec *d, int32_t cp)
{
	dmc_vt *t = T(d);
	if (d->maybe_music) {
		d->maybe_music = 0;
		if (music_start(cp)) {
			/* (the CSI's M is the string's too) */
			d->in_music = 1;
			buf_clear(&d->music);
			buf_addc(&d->music, 'M');
		} else {
			vt_delete_lines(t, 1);
			return 0;
		}
	}
	if (!d->in_music)
		return 0;
	if (cp == 0x0e) {
		d->in_music = 0;
		music_play(t, &d->player, buf_str(&d->music));
	} else if (d->music.n > 4096 || cp == 0x1b || cp == 0x18) {
		d->in_music = 0;
	} else {
		utf8_put(&d->music, (uint32_t)cp);
	}
	return cp != 0x1b;
}

static const int pc_to_ansi[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

static void av_attribute(dmc_vt *t, int a)
{
	int fg = pc_to_ansi[a & 7] + (a & 8), bg = pc_to_ansi[(a >> 4) & 7];
	t->style = st_of(fg, bg, a & 0x80 ? A_BLINK : 0);
}

static int av_args(int c)
{
	switch (c) {
	case 0x01: return 1;
	case 0x08: return 2;
	case 0x0a: case 0x0b: return 5;
	case 0x0c: return 3;
	case 0x0d: return 4;
	case 0x19: return 1;
	}
	return 0;
}

static void av_scroll(dmc_vt *t, int up, int n, int top, int left, int bottom,
		      int right)
{
	struct area a;
	int y0 = clampi(top - 1, 0, t->rows - 1), y1 = clampi(bottom - 1, 0, t->rows - 1);
	int x0 = clampi(left - 1, 0, t->cols - 1), x1 = clampi(right - 1, 0, t->cols - 1);
	int lines, k;
	if (y0 > y1 || x0 > x1)
		return;
	lines = y1 - y0 + 1;
	k = n == 0 ? lines : mini(n, lines);
	if (k < lines) {
		if (up) {
			a.top = y0 + k; a.left = x0; a.bottom = y1; a.right = x1;
			rect_copy(t, &a, y0, x0);
		} else {
			a.top = y0; a.left = x0; a.bottom = y1 - k; a.right = x1;
			rect_copy(t, &a, y0 + k, x0);
		}
	}
	if (up) {
		a.top = y1 - k + 1; a.bottom = y1;
	} else {
		a.top = y0; a.bottom = y0 + k - 1;
	}
	a.left = x0;
	a.right = x1;
	rect_erase(t, &a, 0);
}

static void av_area(dmc_vt *t, int attr, uint32_t cp, int rows, int cols)
{
	struct area a;
	av_attribute(t, attr);
	a.top = t->cy;
	a.left = t->cx;
	a.bottom = mini(t->cy + rows, t->rows - 1);
	a.right = mini(t->cx + cols, t->cols - 1);
	if (cp == ' ')
		rect_erase(t, &a, 0);
	else
		rect_fill(t, &a, (int32_t)cp);
}

static void av_run(struct dec *d)
{
	dmc_vt *t = T(d);
	int c = d->av_cmd, *a = d->av_args, k, j;
	d->av_cmd = AV_NONE;
	switch (c) {
	case AV_REPEAT:
		for (k = 0; k < a[1]; k++)
			vt_print(t, (int32_t)cp437_decode(a[0]));
		break;
	case 0x01: av_attribute(t, a[0]); break;
	case 0x02: t->style = st_with_attrs(t->style, st_attrs(t->style) | A_BLINK); break;
	case 0x03: vt_move_cursor(t, t->cx, t->cy - 1, 0); break;
	case 0x04: vt_move_cursor(t, t->cx, t->cy + 1, 0); break;
	case 0x05: vt_move_cursor(t, t->cx - 1, t->cy, 0); break;
	case 0x06: vt_move_cursor(t, t->cx + 1, t->cy, 0); break;
	case 0x07: vt_erase_line(t, 0, 0); break;
	case 0x08: vt_move_cursor(t, a[1] - 1, a[0] - 1, 0); break;
	case 0x09: t->insert_mode = 1; break;
	case 0x0a: case 0x0b:
		av_scroll(t, c == 0x0a, a[0], a[1], a[2], a[3], a[4]);
		break;
	case 0x0c: av_area(t, a[0], ' ', a[1], a[2]); break;
	case 0x0d: av_area(t, a[0], cp437_decode(a[1]), a[2], a[3]); break;
	case 0x0e: vt_delete_chars(t, 1); break;
	case 0x19:
		for (k = 0; k < a[1 + a[0]]; k++)
			for (j = 0; j < a[0]; j++)
				vt_print(t, (int32_t)cp437_decode(a[1 + j]));
		break;
	}
}

static void av_expect(struct dec *d, int c, int n)
{
	d->av_cmd = c;
	d->av_need = n;
	d->av_n = 0;
	d->av_quoted = 0;
	if (n == 0)
		av_run(d);
}

static void av_argument(struct dec *d, int b)
{
	if (d->av_cmd == AV_LEAD) {
		/* (any ^V command but insert mode's own ends insert mode) */
		if (b != 0x09)
			T(d)->insert_mode = 0;
		av_expect(d, b, av_args(b));
		return;
	}
	if (d->av_cmd == 0x01 && b == 0x10 && !d->av_quoted) {
		d->av_quoted = 1;
		return;
	}
	if (d->av_n < (int)(sizeof(d->av_args) / sizeof(d->av_args[0])))
		d->av_args[d->av_n++] = b;
	/* ^V ^Y: its pattern's length, the pattern, then the count */
	if (d->av_cmd == 0x19 && d->av_n == 1)
		d->av_need = 1 + b + 1;
	if (d->av_n >= d->av_need)
		av_run(d);
}

static int avatar_intercept(struct dec *d, int32_t cp)
{
	dmc_vt *t = T(d);
	if (d->av_cmd != AV_NONE) {
		av_argument(d, cp437_encode((uint32_t)cp));
		return 1;
	}
	if (bbs_intercept(d, cp))
		return 1;
	switch (cp) {
	case 0x0c:
		t->insert_mode = 0;
		av_attribute(t, 3);
		vt_erase_display(t, 2, 0);
		vt_move_cursor(t, 0, 0, 0);
		return 1;
	case 0x19:
		av_expect(d, AV_REPEAT, 2);
		return 1;
	case 0x16:
		d->av_cmd = AV_LEAD;
		return 1;
	}
	return 0;
}

static int intercept(struct dec *d, int32_t cp)
{
	if (d->rip && rip_intercept(d->rip, cp))
		return 1;
	if (d->avatar)
		return avatar_intercept(d, cp);
	if (d->bbs)
		return bbs_intercept(d, cp);
	return 0;
}

/* ------------------------------------------------------------------ */
/* the interpreter */

static void process(struct vt_interp *in, int32_t cp)
{
	struct dec *d = (struct dec *)in;
	if (d->sub) {
		d->sub->ops->process(d->sub, cp);
		if (d->sub_done)
			end_sub(d);
		return;
	}
	if (intercept(d, cp))
		return;
	if (d->printer.controller) {
		printer_feed(&d->printer, cp);
		return;
	}
	switch (d->state) {
	case S_GROUND: ground(d, cp); break;
	case S_ESC: esc(d, cp); break;
	case S_ESC_INTER: esc_inter(d, cp); break;
	case S_CSI: csi(d, cp); break;
	case S_OSC:
		if (cp == 0x07) {
			d->osc_bel = 1;
			osc(d);
			d->state = S_GROUND;
		} else if (cp == 0x1b) {
			d->state = S_OSC_ESC;
		} else if (cp == 0x18 || cp == 0x1a) {
			d->state = S_GROUND;
		} else if (d->osc_len < 4096) {
			utf8_put(&d->osc, (uint32_t)cp);
			d->osc_len++;
		}
		break;
	case S_OSC_ESC:
		/* ST (ESC \) ends it; anything else too, then goes on as ESC */
		d->osc_bel = 0;
		osc(d);
		d->state = S_GROUND;
		if (cp != '\\') {
			d->state = S_ESC;
			esc(d, cp);
		}
		break;
	case S_STRING:
		if (cp == 0x07 || cp == 0x18 || cp == 0x1a)
			d->state = S_GROUND;
		else if (cp == 0x1b)
			d->state = S_STRING_ESC;
		break;
	case S_STRING_ESC:
		d->state = S_GROUND;
		if (cp != '\\') {
			d->state = S_ESC;
			esc(d, cp);
		}
		break;
	case S_DCS: dcs_entry(d, cp); break;
	case S_DCS_PASS:
		if (cp == 0x1b) {
			d->state = S_DCS_ESC;
		} else if (cp == 0x9c) {
			dcs_end(d);
			d->state = S_GROUND;
		} else if (cp == 0x18 || cp == 0x1a) {
			dcs_abort(d);
			d->state = S_GROUND;
		} else {
			dcs_put(d, cp);
		}
		break;
	case S_DCS_ESC:
		d->state = S_GROUND;
		if (cp == '\\') {
			dcs_end(d);
		} else {
			dcs_abort(d);
			d->state = S_ESC;
			esc(d, cp);
		}
		break;
	}
}

/* RIP's text given back to the parser */
static void replay(void *ctx, int32_t cp)
{
	process((struct vt_interp *)ctx, cp);
}

static void dec_soft_reset(struct vt_interp *in)
{
	struct dec *d = (struct dec *)in;
	sets_reset(d);
	d->rectangle = 0;
	d->backarrow_bs = 0;
}

/* the PC's 80x25 (RIP's 80x43), and the profile's own colours */
static void bbs_attach(struct dec *d)
{
	dmc_vt *t = T(d);
	int i;
	t->requested_cols = 80;
	t->requested_rows = d->rip ? 43 : 25;
	for (i = 0; i < d->p->ncolors; i++)
		vt_set_color_override(t, i, d->p->colors[i]);
}

static void dec_reset(struct vt_interp *in)
{
	struct dec *d = (struct dec *)in;
	dmc_vt *t = T(d);
	d->level = d->p->level;
	t->eight_bit_replies = 0;
	udk_reset(d);
	d->preferred = G_LATIN1;
	soft_reset(t, &d->soft);
	d->status_active = 0;
	dec_soft_reset(in);
	end_sub(d);
	t->vt52_keys = 0;
	printer_reset(&d->printer);
	d->allow_columns = 0;
	if (d->sixel) {
		sixel_free(d->sixel);
		d->sixel = NULL;
	}
	d->dcs_on = 0;
	if (d->regis)
		regis_reset(d->regis);
	d->in_regis = 0;
	d->state = S_GROUND;
	if (d->bbs) {
		d->music_mode = 2;
		d->in_music = 0;
		d->maybe_music = 0;
		music_reset(&d->player);
		d->doorway = 0;
		if (d->rip)
			rip_reset(d->rip);
		bbs_attach(d);
	}
	d->av_cmd = AV_NONE;
	d->av_n = 0;
}

/* the keys' scan codes, as a PC's BIOS gives them (doorway mode) */
static int scan_code(int k)
{
	switch (k) {
	case DMC_KEY_UP: return 0x48;
	case DMC_KEY_DOWN: return 0x50;
	case DMC_KEY_LEFT: return 0x4b;
	case DMC_KEY_RIGHT: return 0x4d;
	case DMC_KEY_HOME: return 0x47;
	case DMC_KEY_END: return 0x4f;
	case DMC_KEY_PGUP: return 0x49;
	case DMC_KEY_PGDN: return 0x51;
	case DMC_KEY_INSERT: return 0x52;
	case DMC_KEY_DELETE: return 0x53;
	case DMC_KEY_F1: return 0x3b;
	case DMC_KEY_F2: return 0x3c;
	case DMC_KEY_F3: return 0x3d;
	case DMC_KEY_F4: return 0x3e;
	case DMC_KEY_F5: return 0x3f;
	case DMC_KEY_F6: return 0x40;
	case DMC_KEY_F7: return 0x41;
	case DMC_KEY_F8: return 0x42;
	case DMC_KEY_F9: return 0x43;
	case DMC_KEY_F10: return 0x44;
	case DMC_KEY_F11: return 0x85;
	case DMC_KEY_F12: return 0x86;
	}
	return 0;
}

static int dec_key(struct vt_interp *in, int k, int mods, dmc_buf *out)
{
	struct dec *d = (struct dec *)in;
	int shift = mods & M_SHIFT, alt = mods & M_ALT, ctrl = mods & M_CTRL, n;
	if (d->bbs && d->doorway) {
		int code = scan_code(k);
		if (!code)
			return 0;
		buf_addc(out, 0);
		buf_addc(out, (char)code);
		return 1;
	}
	if (d->vt420 && d->level >= 4 && k == DMC_KEY_BACKSPACE &&
	    d->backarrow_bs && !alt && !ctrl) {
		buf_addc(out, 0x08);
		return 1;
	}
	if (shift && !alt && !ctrl && d->level >= 2) {
		n = udk_number(k);
		if (n && d->has_udk[n]) {
			buf_add(out, d->udk[n].p, d->udk[n].n);
			return 1;
		}
	}
	return 0;
}

static int dec_chr(struct vt_interp *in, uint32_t cp, int mods, dmc_buf *out)
{
	static const char letters[] = "QWERTYUIOPASDFGHJKLZXCVBNM1234567890";
	static const uint8_t codes[] = {
		0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
		0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
		0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32,
		0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f, 0x80, 0x81
	};
	struct dec *d = (struct dec *)in;
	const char *at;
	uint32_t c = cp >= 'a' && cp <= 'z' ? cp - 32 : cp;
	if (!d->bbs || !d->doorway || !(mods & M_ALT) || c == 0 || c > 0x7f)
		return 0;
	at = strchr(letters, (int)c);
	if (!at)
		return 0;
	buf_addc(out, 0);
	buf_addc(out, (char)codes[at - letters]);
	return 1;
}

static int dec_tap(struct vt_interp *in, double fx, double fy, dmc_buf *out)
{
	struct dec *d = (struct dec *)in;
	return d->rip ? rip_tap(d->rip, fx, fy, out) : 0;
}

static void dec_flush(struct vt_interp *in)
{
	struct dec *d = (struct dec *)in;
	if (d->sub && d->sub->ops->flush)
		d->sub->ops->flush(d->sub);
	if (d->regis)
		regis_flush(d->regis);
	if (d->rip)
		rip_flush(d->rip);
}

static void dec_detach(struct vt_interp *in)
{
	struct dec *d = (struct dec *)in;
	dmc_vt *t = T(d);
	if (d->sub && d->sub->ops->detach)
		d->sub->ops->detach(d->sub);
	if (t->soft_glyphs == &d->soft.font)
		t->soft_glyphs = NULL;
	if (d->bbs) {
		t->requested_cols = 0;
		t->requested_rows = 0;
		if (d->p->ncolors)
			vt_reset_colors(t, 0, 15);
		t->ice_colors = 0;
		t->emulated_bps = 0;
	}
}

static void dec_destroy(struct vt_interp *in)
{
	struct dec *d = (struct dec *)in;
	int i;
	if (in->t->soft_glyphs == &d->soft.font)
		in->t->soft_glyphs = NULL;
	if (d->sub)
		interp_free(d->sub);
	printer_free(&d->printer);
	buf_free(&d->osc);
	buf_free(&d->dcs);
	buf_free(&d->music);
	for (i = 0; i < NUDK; i++)
		buf_free(&d->udk[i]);
	if (d->sixel)
		sixel_free(d->sixel);
	if (d->regis)
		regis_free(d->regis);
	if (d->rip)
		rip_free(d->rip);
	free(d->registers);
	free(d);
}

static const struct vt_interp_ops dec_ops = {
	process, dec_reset, dec_soft_reset, save_sets, restore_sets, dec_key,
	dec_chr, dec_tap, dec_flush, dec_detach, dec_destroy
};

struct vt_interp *dec_new(dmc_vt *t, const struct vt_profile *prof)
{
	struct dec *d = (struct dec *)calloc(1, sizeof(*d));
	if (!d)
		return NULL;
	d->in.ops = &dec_ops;
	d->in.t = t;
	d->p = prof;
	d->vt420 = prof->kind == K_DEC && prof->level >= 4;
	d->bbs = prof->kind == K_ANSI_BBS || prof->kind == K_AVATAR ||
		prof->kind == K_RIP;
	d->avatar = prof->kind == K_AVATAR;
	d->level = prof->level;
	d->preferred = G_LATIN1;
	sets_reset(d);
	printer_init(&d->printer, t, "\x1b[4i", "\xc2\x9b" "4i");
	if (prof->features & F_REGIS) {
		d->registers = (uint32_t *)malloc(256 * sizeof(uint32_t));
		if (d->registers)
			sixel_default_palette(d->registers);
	}
	d->av_cmd = AV_NONE;
	d->music_mode = 2;
	music_reset(&d->player);
	if (prof->kind == K_RIP)
		d->rip = rip_new(t, replay, d);
	if (d->bbs)
		bbs_attach(d);
	return &d->in;
}
