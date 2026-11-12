/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * vt_machine.c - the home computers and videotex terminals: what a
 * machine's screen editor was, as a terminal (the app's
 * ByteControlInterpreter): octets read as they come, in the machine's own
 * set, each a control code or a character written in its colour and
 * reverse.  Commodore's PETSCII (PetsciiInterpreter), Atari's ATASCII
 * (AtasciiInterpreter), Prestel's viewdata (ViewdataInterpreter) and the
 * Minitel (MinitelInterpreter, its Téléinformatique mode a VT100); and
 * teletext's mosaics (Mosaic).
 */
#include <stdlib.h>
#include <string.h>

#include "vtint.h"

/* ------------------------------------------------------------------ */
/* the mosaics */

uint32_t mosaic_code(int mask, int separated)
{
	if ((mask & 0x3f) == 0)
		return ' ';
	return MOSAIC_BASE + (uint32_t)(mask & 0x3f) + (separated ? 0x40 : 0);
}

int mosaic_bits(int c)
{
	return (c & 0x1f) | ((c & 0x40) >> 1);
}

/* ------------------------------------------------------------------ */
/* what the machines share */

enum { MK_PETSCII, MK_ATASCII, MK_VIEWDATA, MK_MINITEL };

/* what a Minitel's half-read sequence is waiting for */
enum { X_NONE, X_ESC, X_PRO, X_US, X_REP, X_SEP, X_SS2, X_ACCENT, X_CSI };

#define VD_ROWS 24
#define VD_COLS 40

struct mach {
	struct vt_interp in;
	const struct vt_profile *m;
	int kind;
	int color, reverse, extra_attrs;
	uint8_t tabs[256];

	/* PETSCII's */
	int lower_case, esc;
	/* ATASCII's */
	int literal;
	/* Viewdata's: the codes on the screen */
	uint8_t cells[VD_ROWS][VD_COLS];
	int escaped;
	/* the Minitel's */
	int bg, next_bg, flash, conceal, separated, next_separated, mosaic;
	int scrolling, mixte, last;
	uint8_t pending[4];
	int npending, want, expecting, accent;
	char csi[32];
	int ncsi;
	struct vt_interp *teleinfo;
	int exit_match;
};

#define T(x) ((x)->in.t)

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static vt_style look(const struct mach *x)
{
	if (x->kind == MK_MINITEL)
		return st_of(x->color, x->bg,
			     (x->reverse ? A_INVERSE : 0) | (x->flash ? A_BLINK : 0) |
			     (x->conceal ? A_INVISIBLE : 0));
	return st_of(x->color, ST_DEFAULT_BG,
		     (x->reverse ? A_INVERSE : 0) | x->extra_attrs);
}

static void reset_tabs(struct mach *x, int every)
{
	int i;
	for (i = 0; i < 256; i++)
		x->tabs[i] = (uint8_t)(every && i % 8 == 0 && i > 0);
}

static void attach(struct mach *x)
{
	dmc_vt *t = T(x);
	vt_use_machine(t, x->m->columns, x->m->rows, x->m->colors, x->m->ncolors,
		       x->m->default_color, x->m->background);
	t->autowrap = 1;
	t->style = look(x);
}

static void color_to(struct mach *x, int c)
{
	x->color = c;
	T(x)->style = look(x);
}

static void reverse_to(struct mach *x, int on)
{
	x->reverse = on;
	T(x)->style = look(x);
}

static void attr_to(struct mach *x, int a, int on)
{
	x->extra_attrs = on ? x->extra_attrs | a : x->extra_attrs & ~a;
	T(x)->style = look(x);
}

static void new_line(struct mach *x, int wrapped)
{
	dmc_vt *t = T(x);
	if (wrapped)
		vt_line(t, t->cy)->wrapped = 1;
	vt_carriage_return(t);
	vt_index(t);
}

static void put(struct mach *x, uint32_t cp)
{
	dmc_vt *t = T(x);
	t->style = look(x);
	vt_print(t, (int32_t)cp);
	if (t->pending_wrap)
		new_line(x, 1);
}

static void up(dmc_vt *t)
{
	vt_move_cursor(t, t->cx, t->cy - 1, 0);
}

static void down(dmc_vt *t)
{
	int cx = t->cx;
	vt_index(t);
	vt_move_cursor(t, cx, t->cy, 0);
}

static void left(dmc_vt *t)
{
	if (t->cx > 0)
		vt_move_cursor(t, t->cx - 1, t->cy, 0);
	else if (t->cy > 0)
		vt_move_cursor(t, t->cols - 1, t->cy - 1, 0);
}

static void right(struct mach *x)
{
	dmc_vt *t = T(x);
	if (t->cx < t->cols - 1)
		vt_move_cursor(t, t->cx + 1, t->cy, 0);
	else
		new_line(x, 0);
}

static void home(dmc_vt *t)
{
	vt_move_cursor(t, 0, 0, 0);
}

static void clear(struct mach *x)
{
	dmc_vt *t = T(x);
	t->style = st_erased(look(x));
	vt_erase_display(t, 2, 0);
	home(t);
	t->style = look(x);
}

static void delete_back(struct mach *x)
{
	dmc_vt *t = T(x);
	if (t->cx == 0 && t->cy == 0)
		return;
	left(t);
	t->style = st_erased(look(x));
	vt_delete_chars(t, 1);
	t->style = look(x);
}

static void insert_blank(struct mach *x)
{
	dmc_vt *t = T(x);
	t->style = st_erased(look(x));
	vt_insert_chars(t, 1);
	t->style = look(x);
}

static void tab(struct mach *x)
{
	dmc_vt *t = T(x);
	int c = t->cx + 1;
	while (c < t->cols - 1 && c < 256 && !x->tabs[c])
		c++;
	vt_move_cursor(t, c < t->cols - 1 ? c : t->cols - 1, t->cy, 0);
}

static void tab_stop(struct mach *x, int on)
{
	if (T(x)->cx >= 0 && T(x)->cx < 256)
		x->tabs[T(x)->cx] = (uint8_t)on;
}

/* code point cp's first code from..to in set (256 code points), or -1 */
static int code_in(const uint32_t *set, uint32_t cp, int from, int to)
{
	int b;
	for (b = from; b <= to; b++)
		if (set[b] == cp)
			return b;
	return -1;
}

static void put_byte(dmc_buf *out, int b)
{
	buf_addc(out, (char)b);
}

/* a keypad key's character (-1: not one) */
static int keypad_char(int k)
{
	if (k >= DMC_KEY_KP_0 && k <= DMC_KEY_KP_9)
		return '0' + (k - DMC_KEY_KP_0);
	if (k == DMC_KEY_KP_MINUS)
		return '-';
	if (k == DMC_KEY_KP_COMMA)
		return ',';
	if (k == DMC_KEY_KP_PERIOD)
		return '.';
	return -1;
}

/* ------------------------------------------------------------------ */
/* PETSCII */

static int color_code(const struct vt_profile *m, int b)
{
	int i;
	for (i = 0; i < m->n_ctrl; i++)
		if (m->ctrl_colors[i] == b)
			return i;
	for (i = 0; i < m->n_cbm; i++)
		if (m->cbm_colors[i] == b)
			return 8 + i;
	return -1;
}

static int attr_code(struct mach *x, int b)
{
	if (x->m->attr_codes == AC_VDC) {
		switch (b) {
		case 0x02: attr_to(x, A_UNDERLINE, 1); return 1;
		case 0x82: attr_to(x, A_UNDERLINE, 0); return 1;
		case 0x0f: attr_to(x, A_BLINK, 1); return 1;
		case 0x8f: attr_to(x, A_BLINK, 0); return 1;
		}
	} else if (x->m->attr_codes == AC_PLUS4) {
		switch (b) {
		case 0x82: attr_to(x, A_BLINK, 1); return 1;
		case 0x84: attr_to(x, A_BLINK, 0); return 1;
		}
	}
	return 0;
}

static int end_of_line(dmc_vt *t)
{
	struct vt_line *l = vt_line(t, t->cy);
	int c = t->cols - 1;
	while (c > 0 && (l->ch[c] == 0 || l->ch[c] == ' '))
		c--;
	return c + 1 < t->cols - 1 ? c + 1 : t->cols - 1;
}

static void pet_escape(struct mach *x, int c)
{
	dmc_vt *t = T(x);
	switch (c) {
	case '@': vt_erase_display(t, 0, 0); break;
	case 'A': t->insert_mode = 1; break;
	case 'C': t->insert_mode = 0; break;
	case 'D': vt_delete_lines(t, 1); break;
	case 'I': vt_insert_lines(t, 1); break;
	case 'J': vt_move_cursor(t, 0, t->cy, 0); break;
	case 'K': vt_move_cursor(t, end_of_line(t), t->cy, 0); break;
	case 'O': t->insert_mode = 0; reverse_to(x, 0); break;
	case 'P': vt_erase_line(t, 1, 0); break;
	case 'Q': vt_erase_line(t, 0, 0); break;
	case 'T': vt_set_region(t, t->cy, t->bottom); break;
	case 'B': vt_set_region(t, t->top, t->cy); break;
	case 'V': vt_scroll_up(t, t->top, t->bottom); break;
	case 'W': vt_scroll_down(t, t->top, t->bottom); break;
	case 'N': t->reverse_video = 0; t->changed = 1; break;
	case 'R': t->reverse_video = 1; t->changed = 1; break;
	case 'Y': reset_tabs(x, 1); break;
	case 'Z': reset_tabs(x, 0); break;
	}
}

static int pet_control(struct mach *x, int b)
{
	dmc_vt *t = T(x);
	int c;
	if (x->esc) {
		x->esc = 0;
		pet_escape(x, b);
		return 1;
	}
	if ((c = color_code(x->m, b)) >= 0) {
		color_to(x, c);
		return 1;
	}
	if (attr_code(x, b))
		return 1;
	switch (b) {
	case 0x07: vt_bell(t); break;
	case 0x09: if (x->m->tab_lf) tab(x); break;
	case 0x0a: if (x->m->tab_lf) down(t); break;
	case 0x0d: case 0x8d: new_line(x, 0); reverse_to(x, 0); break;
	case 0x0e: x->lower_case = 1; break;
	case 0x8e: x->lower_case = 0; break;
	case 0x11: down(t); break;
	case 0x91: up(t); break;
	case 0x1d: right(x); break;
	case 0x9d: left(t); break;
	case 0x12: reverse_to(x, 1); break;
	case 0x92: reverse_to(x, 0); break;
	case 0x13: home(t); break;
	case 0x93: clear(x); break;
	case 0x14: delete_back(x); break;
	case 0x94: insert_blank(x); break;
	case 0x1b: x->esc = x->m->escapes; break;
	default:
		return b < 0x20 || (b >= 0x80 && b <= 0x9f);
	}
	return 1;
}

static const uint32_t *pet_set(const struct mach *x)
{
	return x->lower_case ? x->m->lower : x->m->upper;
}

static int pet_key(struct mach *x, int k, int shift, dmc_buf *out)
{
	int b, kp = keypad_char(k);
	(void)x;
	switch (k) {
	case DMC_KEY_UP: b = 0x91; break;
	case DMC_KEY_DOWN: b = 0x11; break;
	case DMC_KEY_LEFT: b = 0x9d; break;
	case DMC_KEY_RIGHT: b = 0x1d; break;
	case DMC_KEY_HOME: b = shift ? 0x93 : 0x13; break;
	case DMC_KEY_INSERT: b = 0x94; break;
	case DMC_KEY_BACKSPACE: case DMC_KEY_DELETE: b = shift ? 0x94 : 0x14; break;
	case DMC_KEY_ENTER: case DMC_KEY_KP_ENTER: b = shift ? 0x8d : 0x0d; break;
	case DMC_KEY_ESCAPE: b = 0x03; break;
	case DMC_KEY_TAB: b = 0x09; break;
	case DMC_KEY_F1: b = 0x85; break;
	case DMC_KEY_F3: b = 0x86; break;
	case DMC_KEY_F5: b = 0x87; break;
	case DMC_KEY_F7: b = 0x88; break;
	case DMC_KEY_F2: b = 0x89; break;
	case DMC_KEY_F4: b = 0x8a; break;
	case DMC_KEY_F6: b = 0x8b; break;
	case DMC_KEY_F8: b = 0x8c; break;
	default:
		if (kp < 0)
			return 1;
		b = kp;
	}
	put_byte(out, b);
	return 1;
}

/* C= and A-Z: the graphics on the keys' left fronts */
static const uint8_t cbm_graphics[26] = {
	0xb0, 0xbf, 0xbc, 0xac, 0xb1, 0xbb, 0xa5, 0xb4, 0xa2, 0xb5, 0xa1, 0xb6, 0xa7,
	0xaa, 0xb9, 0xaf, 0xab, 0xb2, 0xae, 0xa3, 0xb8, 0xbe, 0xb3, 0xbd, 0xb7, 0xad
};

static int pet_code(struct mach *x, uint32_t cp, int alt, int ctrl)
{
	const struct vt_profile *m = x->m;
	uint32_t upper = cp >= 'a' && cp <= 'z' ? cp - 32 : cp;
	int b;
	if (ctrl) {
		if (cp >= '1' && cp <= '8')
			return (int)(cp - '1') < m->n_ctrl ? m->ctrl_colors[cp - '1'] : -1;
		if (cp == '9')
			return 0x12;
		if (cp == '0')
			return 0x92;
		if (upper >= 'A' && upper <= 'Z')
			return (int)(upper - '@');
	}
	if (alt) {
		if (cp >= '1' && cp <= '8')
			return (int)(cp - '1') < m->n_cbm ? m->cbm_colors[cp - '1'] : -1;
		if (upper >= 'A' && upper <= 'Z')
			return cbm_graphics[upper - 'A'];
	}
	if (cp == '\r' || cp == '\n')
		return 0x0d;
	if (cp == 0x08 || cp == 0x7f)
		return 0x14;
	if (cp == 0x1b)
		return 0x03;
	if (cp >= 'a' && cp <= 'z')
		return (int)(cp - 32);
	if (cp >= 'A' && cp <= 'Z')
		return x->lower_case ? (int)cp + 0x80 : (int)cp;
	if (cp == '^')
		return 0x5e;
	if (cp == '_')
		return 0x5f;
	if ((b = code_in(pet_set(x), cp, 0x20, 0x5f)) >= 0 ||
	    (b = code_in(pet_set(x), cp, 0xa0, 0xdf)) >= 0 ||
	    (b = code_in(pet_set(x), cp, 0, 0xff)) >= 0)
		return b;
	return '?';
}

/* ------------------------------------------------------------------ */
/* ATASCII */

static int atari_controls(int b)
{
	switch (b) {
	case 0x1b: case 0x1c: case 0x1d: case 0x1e: case 0x1f: case 0x7d:
	case 0x7e: case 0x7f: case 0x9b: case 0x9c: case 0x9d: case 0x9e:
	case 0x9f: case 0xfd: case 0xfe: case 0xff:
		return 1;
	}
	return 0;
}

static void atari_show(struct mach *x, int b)
{
	int inverse = b >= 0x80;
	if (inverse != x->reverse)
		reverse_to(x, inverse);
	put(x, x->m->atascii[b & 0x7f]);
}

static int atari_control(struct mach *x, int b)
{
	dmc_vt *t = T(x);
	switch (b) {
	case 0x1b: x->literal = 1; break;
	case 0x1c: vt_move_cursor(t, t->cx, t->cy == 0 ? t->rows - 1 : t->cy - 1, 0); break;
	case 0x1d: vt_move_cursor(t, t->cx, t->cy == t->rows - 1 ? 0 : t->cy + 1, 0); break;
	case 0x1e: vt_move_cursor(t, t->cx == 0 ? t->cols - 1 : t->cx - 1, t->cy, 0); break;
	case 0x1f: vt_move_cursor(t, t->cx == t->cols - 1 ? 0 : t->cx + 1, t->cy, 0); break;
	case 0x7d: clear(x); break;
	case 0x7e:
		/* BACK S: the cursor left, the character there blanked */
		if (t->cx == 0)
			break;
		vt_move_cursor(t, t->cx - 1, t->cy, 0);
		t->style = st_erased(look(x));
		vt_erase_chars(t, 1);
		t->style = look(x);
		break;
	case 0x7f: tab(x); break;
	case 0x9b: new_line(x, 0); break;
	case 0x9c: vt_delete_lines(t, 1); break;
	case 0x9d: vt_insert_lines(t, 1); break;
	case 0x9e: tab_stop(x, 0); break;
	case 0x9f: tab_stop(x, 1); break;
	case 0xfd: vt_bell(t); break;
	case 0xfe: vt_delete_chars(t, 1); break;
	case 0xff: insert_blank(x); break;
	default:
		return 0;
	}
	return 1;
}

static int atari_key(int k, int shift, dmc_buf *out)
{
	int b, kp = keypad_char(k);
	switch (k) {
	case DMC_KEY_UP: b = 0x1c; break;
	case DMC_KEY_DOWN: b = 0x1d; break;
	case DMC_KEY_LEFT: b = 0x1e; break;
	case DMC_KEY_RIGHT: b = 0x1f; break;
	case DMC_KEY_ENTER: case DMC_KEY_KP_ENTER: b = 0x9b; break;
	case DMC_KEY_BACKSPACE: b = shift ? 0x9c : 0x7e; break;
	case DMC_KEY_DELETE: b = shift ? 0x9c : 0xfe; break;
	case DMC_KEY_INSERT: b = shift ? 0x9d : 0xff; break;
	case DMC_KEY_TAB: b = shift ? 0x9f : 0x7f; break;
	case DMC_KEY_ESCAPE: b = 0x1b; break;
	case DMC_KEY_HOME:
		if (!shift)
			return 1;
		b = 0x7d;
		break;
	default:
		if (kp < 0)
			return 1;
		b = kp;
	}
	put_byte(out, b);
	return 1;
}

static int atari_code(struct mach *x, uint32_t cp, int ctrl)
{
	int b;
	if (ctrl) {
		uint32_t upper = cp >= 'a' && cp <= 'z' ? cp - 32 : cp;
		if (upper >= 'A' && upper <= 'Z')
			return (int)(upper - '@');
		if (cp == ',')
			return 0x00;
		if (cp == '.')
			return 0x60;
		if (cp == ';')
			return 0x7b;
	}
	if (cp == '\r' || cp == '\n')
		return 0x9b;
	if (cp == 0x08 || cp == 0x7f)
		return 0x7e;
	if (cp == 0x1b)
		return 0x1b;
	if ((b = code_in(x->m->atascii, cp, 0x20, 0x7c)) >= 0 ||
	    (b = code_in(x->m->atascii, cp, 0x00, 0x1a)) >= 0)
		return b;
	return '?';
}

/* ------------------------------------------------------------------ */
/* the videotex terminals' cursor, round the screen */

static int first_row(const struct mach *x)
{
	return x->kind == MK_MINITEL ? 1 : 0;
}

static void at(dmc_vt *t, int cx, int cy)
{
	vt_move_cursor(t, cx, cy, 0);
}

static void line_down(struct mach *x)
{
	dmc_vt *t = T(x);
	if (x->kind == MK_MINITEL && t->cy >= t->rows - 1 && x->scrolling) {
		int cx = t->cx;
		vt_index(t);
		at(t, cx, t->cy);
		return;
	}
	at(t, t->cx, t->cy < t->rows - 1 ? t->cy + 1 : first_row(x));
}

static void line_up(struct mach *x)
{
	dmc_vt *t = T(x);
	at(t, t->cx, t->cy > first_row(x) ? t->cy - 1 : t->rows - 1);
}

static void back(struct mach *x)
{
	dmc_vt *t = T(x);
	if (t->cx > 0)
		at(t, t->cx - 1, t->cy);
	else
		at(t, t->cols - 1, t->cy > first_row(x) ? t->cy - 1 : t->rows - 1);
}

static void forward(struct mach *x)
{
	dmc_vt *t = T(x);
	if (t->cx < t->cols - 1) {
		at(t, t->cx + 1, t->cy);
	} else {
		at(t, 0, t->cy);
		line_down(x);
	}
}

/* ------------------------------------------------------------------ */
/* Viewdata */

static vt_style vd_look(int fg, int bg, int flash, int conceal)
{
	return st_of(fg, bg, (flash ? A_BLINK : 0) | (conceal ? A_INVISIBLE : 0));
}

/* row y drawn from its codes, as a teletext decoder does */
static void vd_draw(struct mach *x, int y)
{
	dmc_vt *t = T(x);
	struct vt_line *line;
	int fg = 7, bg = 0, mosaic = 0, separated = 0, hold = 0, flash = 0;
	int conceal = 0, c, cx;
	uint32_t held = ' ', shown;
	if (y >= t->rows || y >= VD_ROWS)
		return;
	line = vt_line(t, y);
	for (cx = 0; cx < t->cols && cx < VD_COLS; cx++) {
		c = x->cells[y][cx];
		if (c < 0x20) {
			/* set-at */
			switch (c) {
			case 0x09: flash = 0; break;
			case 0x0c: held = ' '; break;
			case 0x18: conceal = 1; break;
			case 0x19: separated = 0; break;
			case 0x1a: separated = 1; break;
			case 0x1c: bg = 0; break;
			case 0x1d: bg = fg; break;
			case 0x1e: hold = 1; break;
			}
			shown = hold && mosaic ? held : ' ';
			line_set(line, cx, (int32_t)shown, vd_look(fg, bg, flash, conceal));
			/* set-after */
			if (c >= 0x01 && c <= 0x07) {
				fg = c;
				if (mosaic)
					held = ' ';
				mosaic = 0;
				conceal = 0;
			} else if (c >= 0x11 && c <= 0x17) {
				fg = c - 0x10;
				mosaic = 1;
				conceal = 0;
			} else if (c == 0x08) {
				flash = 1;
			} else if (c == 0x1f) {
				hold = 0;
			}
			continue;
		}
		if (mosaic && (c < 0x40 || c >= 0x60)) {
			shown = mosaic_code(mosaic_bits(c), separated);
			held = shown;
		} else {
			shown = viewdata_g0(c);
		}
		line_set(line, cx, (int32_t)shown, vd_look(fg, bg, flash, conceal));
	}
	t->changed = 1;
}

static void vd_store(struct mach *x, int c)
{
	dmc_vt *t = T(x);
	if (t->cy >= 0 && t->cy < VD_ROWS && t->cx >= 0 && t->cx < VD_COLS) {
		x->cells[t->cy][t->cx] = (uint8_t)c;
		vd_draw(x, t->cy);
	}
	forward(x);
}

static void vd_blank(struct mach *x)
{
	memset(x->cells, ' ', sizeof(x->cells));
}

static int vd_control(struct mach *x, int b)
{
	dmc_vt *t = T(x);
	int y;
	switch (b) {
	case 0x08: back(x); break;
	case 0x09: forward(x); break;
	case 0x0a: line_down(x); break;
	case 0x0b: line_up(x); break;
	case 0x0c:
		vd_blank(x);
		for (y = 0; y < t->rows && y < VD_ROWS; y++)
			vd_draw(x, y);
		home(t);
		break;
	case 0x0d: at(t, 0, t->cy); break;
	case 0x1e: home(t); break;
	case 0x11: t->cursor_visible = 1; break;
	case 0x14: t->cursor_visible = 0; break;
	case 0x1b: x->escaped = 1; break;
	default:
		return b < 0x20;
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* the Minitel */

static const char accented_base[] = "aeuioAEUIO";
static const int typed_accent[4] = { 0x41, 0x42, 0x43, 0x48 };
static const uint32_t typed_letters[4][10] = {
	{ 0xe0, 0xe8, 0xf9, 0xec, 0xf2, 0xc0, 0xc8, 0xd9, 0xcc, 0xd2 },
	{ 0xe1, 0xe9, 0xfa, 0xed, 0xf3, 0xc1, 0xc9, 0xda, 0xcd, 0xd3 },
	{ 0xe2, 0xea, 0xfb, 0xee, 0xf4, 0xc2, 0xca, 0xdb, 0xce, 0xd4 },
	{ 0xe4, 0xeb, 0xfc, 0xef, 0xf6, 0xc4, 0xcb, 0xdc, 0xcf, 0xd6 }
};
/* G2's symbols, by SS2's code */
static const uint32_t g2[][2] = {
	{ 0x23, 0xa3 }, { 0x24, '$' }, { 0x26, '#' }, { 0x27, 0xa7 },
	{ 0x2c, 0x2190 }, { 0x2d, 0x2191 }, { 0x2e, 0x2192 }, { 0x2f, 0x2193 },
	{ 0x30, 0xb0 }, { 0x31, 0xb1 }, { 0x38, 0xf7 },
	{ 0x3c, 0xbc }, { 0x3d, 0xbd }, { 0x3e, 0xbe },
	{ 0x6a, 0x152 }, { 0x7a, 0x153 }, { 0x7b, 0xdf }
};
#define N_G2 ((int)(sizeof(g2) / sizeof(g2[0])))

static void mt_attributes(struct mach *x)
{
	x->color = 7;
	x->bg = 0;
	x->next_bg = 0;
	x->flash = 0;
	x->conceal = 0;
	x->reverse = 0;
	x->separated = 0;
	x->next_separated = 0;
	x->mosaic = 0;
	T(x)->style = look(x);
}

static void mt_show(struct mach *x, int b)
{
	dmc_vt *t = T(x);
	int is_mosaic = x->mosaic && b < 0x80 && (b < 0x40 || b >= 0x60);
	uint32_t c;
	if (is_mosaic || b == ' ') {
		x->bg = x->next_bg;
		x->separated = x->next_separated;
	}
	c = is_mosaic ? mosaic_code(mosaic_bits(b), x->separated) : (uint32_t)b;
	x->last = b;
	t->style = look(x);
	vt_print(t, (int32_t)c);
	if (t->pending_wrap) {
		vt_carriage_return(t);
		line_down(x);
	}
}

static int mt_control(struct mach *x, int b)
{
	dmc_vt *t = T(x);
	switch (b) {
	case 0x00: break;
	case 0x7f: if (x->mosaic) mt_show(x, b); break;
	case 0x07: vt_bell(t); break;
	case 0x08: back(x); break;
	case 0x09: forward(x); break;
	case 0x0a: line_down(x); break;
	case 0x0b: line_up(x); break;
	case 0x0c:
		clear(x);
		mt_attributes(x);
		at(t, 0, 1);
		break;
	case 0x0d: at(t, 0, t->cy); break;
	case 0x0e:
		if (!x->mixte) {
			x->mosaic = 1;
			x->separated = 0;
			x->next_separated = 0;
		}
		break;
	case 0x0f: x->mosaic = 0; break;
	case 0x11: t->cursor_visible = 1; break;
	case 0x14: t->cursor_visible = 0; break;
	case 0x12: x->expecting = X_REP; break;
	case 0x13: x->expecting = X_SEP; break;
	case 0x18:
		t->style = st_erased(look(x));
		vt_erase_line(t, 0, 0);
		t->style = look(x);
		break;
	case 0x19: x->expecting = X_SS2; break;
	case 0x1b: x->expecting = X_ESC; break;
	case 0x1e:
		mt_attributes(x);
		at(t, 0, 1);
		break;
	case 0x1f:
		x->npending = 0;
		x->expecting = X_US;
		break;
	default:
		return b < 0x20;
	}
	return 1;
}

static void mt_esc(struct mach *x, int b)
{
	dmc_vt *t = T(x);
	char s[4];
	if (b >= 0x40 && b <= 0x47)
		x->color = b - 0x40;
	else if (b >= 0x50 && b <= 0x57)
		x->next_bg = b - 0x50;
	else
		switch (b) {
		case 0x48: x->flash = 1; break;
		case 0x49: x->flash = 0; break;
		case 0x58: x->conceal = 1; break;
		case 0x5f: x->conceal = 0; break;
		case 0x59: x->next_separated = 0; break;
		case 0x5a: x->next_separated = 1; break;
		case 0x5c: x->reverse = 0; break;
		case 0x5d: x->reverse = 1; break;
		case 0x5b:
			x->ncsi = 0;
			x->expecting = X_CSI;
			break;
		case 0x39: case 0x3a: case 0x3b:
			x->npending = 0;
			x->want = b - 0x38;
			x->expecting = X_PRO;
			break;
		case 0x61:
			s[0] = 0x1f;
			s[1] = (char)(0x40 + t->cy);
			s[2] = (char)(0x41 + t->cx);
			s[3] = 0;
			vt_reply(t, s);
			break;
		}
	t->style = look(x);
}

static void mt_protocol(struct mach *x)
{
	dmc_vt *t = T(x);
	int a = x->pending[0], b = x->npending > 1 ? x->pending[1] : 0;
	if (x->npending == 1 && a == 0x7b) {
		vt_reply(t, "\x01" "Cv;\x04");
	} else if (x->npending == 2 && a == 0x69 && b == 0x43) {
		x->scrolling = 1;
	} else if (x->npending == 2 && a == 0x6a && b == 0x43) {
		x->scrolling = 0;
	} else if (x->npending == 2 && a == 0x32 && b == 0x7d) {
		x->mixte = 1;
		x->mosaic = 0;
		t->requested_cols = 80;
	} else if (x->npending == 2 && a == 0x32 && b == 0x7e) {
		x->mixte = 0;
		t->requested_cols = x->m->columns;
	} else if (x->npending == 2 && a == 0x31 && b == 0x7d) {
		/* Téléinformatique: an 80x24 VT100 */
		t->requested_cols = 80;
		t->requested_rows = 24;
		if (x->teleinfo)
			interp_free(x->teleinfo);
		x->teleinfo = dec_new(t, &vt_profile_vt100);
		x->exit_match = 0;
		t->style = ST_NORMAL;
	}
}

static void mt_command(struct mach *x, int final)
{
	dmc_vt *t = T(x);
	int ps[8], n = 0, have0, i = 0;
	const char *s = x->csi;
	memset(ps, 0, sizeof(ps));
	x->csi[x->ncsi] = 0;
	/* the parameters split at ';' (what isn't a number 0) */
	for (;;) {
		int v = 0, ok = 1, any = 0;
		while (*s && *s != ';') {
			if (*s >= '0' && *s <= '9') {
				if (v < 10000)
					v = v * 10 + (*s - '0');
				any = 1;
			} else {
				ok = 0;
			}
			s++;
		}
		if (n < 8)
			ps[n++] = ok && any ? v : 0;
		if (!*s)
			break;
		s++;
	}
	have0 = x->ncsi > 0 || n > 1;
#define PD(k, dflt) ((k) < n && ps[k] ? ps[k] : (dflt))
	t->style = st_erased(look(x));
	switch (final) {
	case 'A': i = t->cy - PD(0, 1); at(t, t->cx, i > 1 ? i : 1); break;
	case 'B': at(t, t->cx, t->cy + PD(0, 1)); break;
	case 'C': at(t, t->cx + PD(0, 1), t->cy); break;
	case 'D': at(t, t->cx - PD(0, 1), t->cy); break;
	case 'H': at(t, PD(1, 1) - 1, PD(0, 1)); break;
	case 'J': vt_erase_display(t, ps[0], 0); break;
	case 'K': vt_erase_line(t, ps[0], 0); break;
	case 'P': vt_delete_chars(t, PD(0, 1)); break;
	case '@': vt_insert_chars(t, PD(0, 1)); break;
	case 'L': vt_insert_lines(t, PD(0, 1)); break;
	case 'M': vt_delete_lines(t, PD(0, 1)); break;
	case 'h': if (have0 && ps[0] == 4) t->insert_mode = 1; break;
	case 'l': if (have0 && ps[0] == 4) t->insert_mode = 0; break;
	}
#undef PD
	t->style = look(x);
}

static uint32_t mt_accented(int b, int a)
{
	const char *p;
	int i, k;
	if (a == 0x4b)
		return b == 'c' ? 0xe7 : b == 'C' ? 0xc7 : (uint32_t)b;
	p = b > 0 && b < 0x80 ? strchr(accented_base, b) : NULL;
	if (!p)
		return (uint32_t)b;
	i = (int)(p - accented_base);
	for (k = 0; k < 4; k++)
		if (typed_accent[k] == a)
			return typed_letters[k][i];
	return (uint32_t)b;
}

static void mt_sequence(struct mach *x, int b)
{
	dmc_vt *t = T(x);
	int e = x->expecting, k;
	x->expecting = X_NONE;
	switch (e) {
	case X_ESC: mt_esc(x, b); break;
	case X_PRO:
		x->pending[x->npending++] = (uint8_t)b;
		if (x->npending < x->want)
			x->expecting = X_PRO;
		else
			mt_protocol(x);
		break;
	case X_US:
		x->pending[x->npending++] = (uint8_t)b;
		if (x->npending < 2) {
			x->expecting = X_US;
		} else {
			int a = x->pending[0], c = x->pending[1], row, col;
			if (a >= 0x30 && a <= 0x32) {
				row = (a - 0x30) * 10 + (c - 0x30);
				col = 1;
			} else {
				row = a - 0x40;
				col = c - 0x40;
			}
			mt_attributes(x);
			at(t, clampi(col - 1, 0, t->cols - 1), clampi(row, 0, t->rows - 1));
		}
		break;
	case X_REP:
		for (k = clampi(b - 0x40, 0, 63); k > 0; k--)
			mt_show(x, x->last);
		break;
	case X_SEP: break;
	case X_SS2:
		if (b >= 0x41 && b <= 0x4b) {
			x->accent = b;
			x->expecting = X_ACCENT;
			break;
		}
		for (k = 0; k < N_G2; k++)
			if ((int)g2[k][0] == b)
				mt_show(x, (int)g2[k][1]);
		break;
	case X_ACCENT:
		mt_show(x, (int)mt_accented(b, x->accent));
		break;
	case X_CSI:
		if (b >= 0x30 && b <= 0x3f) {
			if (x->ncsi < (int)sizeof(x->csi) - 1)
				x->csi[x->ncsi++] = (char)b;
			x->expecting = X_CSI;
		} else {
			mt_command(x, b);
		}
		break;
	}
}

/* whether b ends ESC [ ? { (Téléinformatique mode left) */
static int leaves_teleinfo(struct mach *x, int b)
{
	static const char exit_seq[] = "\x1b[?{";
	dmc_vt *t = T(x);
	x->exit_match = b == exit_seq[x->exit_match] ? x->exit_match + 1 :
		b == 0x1b ? 1 : 0;
	if (x->exit_match < 4)
		return 0;
	x->exit_match = 0;
	interp_free(x->teleinfo);
	x->teleinfo = NULL;
	t->requested_cols = x->m->columns;
	t->requested_rows = x->m->rows;
	clear(x);
	mt_attributes(x);
	at(t, 0, 1);
	return 1;
}

/* ------------------------------------------------------------------ */
/* the interpreter */

static void process(struct vt_interp *in, int32_t cp)
{
	struct mach *x = (struct mach *)in;
	int b = cp & 0xff;
	uint32_t c;
	switch (x->kind) {
	case MK_PETSCII:
		if (pet_control(x, b))
			return;
		c = pet_set(x)[b];
		if (c)
			put(x, c);
		return;
	case MK_ATASCII:
		if (x->literal) {
			x->literal = 0;
			atari_show(x, b);
			return;
		}
		if (!atari_control(x, b))
			atari_show(x, b);
		return;
	case MK_VIEWDATA:
		b &= 0x7f;
		if (x->escaped) {
			x->escaped = 0;
			if (b >= 0x40 && b <= 0x5f)
				vd_store(x, b - 0x40);
			return;
		}
		if (!vd_control(x, b) && b >= 0x20)
			vd_store(x, b);
		return;
	case MK_MINITEL:
		b &= 0x7f;
		if (x->teleinfo) {
			if (!leaves_teleinfo(x, b))
				x->teleinfo->ops->process(x->teleinfo, b);
			return;
		}
		if (x->expecting != X_NONE) {
			mt_sequence(x, b);
			return;
		}
		if (mt_control(x, b))
			return;
		if (b >= 0x20)
			mt_show(x, b);
		return;
	}
}

static void mach_reset(struct vt_interp *in)
{
	struct mach *x = (struct mach *)in;
	dmc_vt *t = T(x);
	x->color = x->m->default_color;
	x->reverse = 0;
	x->extra_attrs = 0;
	reset_tabs(x, 1);
	attach(x);
	switch (x->kind) {
	case MK_PETSCII:
		x->lower_case = 0;
		x->esc = 0;
		break;
	case MK_ATASCII:
		x->literal = 0;
		break;
	case MK_VIEWDATA:
		t->soft_glyphs = &vt_font_mosaic;
		vd_blank(x);
		x->escaped = 0;
		break;
	case MK_MINITEL:
		t->soft_glyphs = &vt_font_mosaic;
		if (x->teleinfo)
			interp_free(x->teleinfo);
		x->teleinfo = NULL;
		x->mixte = 0;
		x->scrolling = 0;
		mt_attributes(x);
		x->expecting = X_NONE;
		at(t, 0, 1);
		break;
	}
}

static void mach_soft_reset(struct vt_interp *in)
{
	struct mach *x = (struct mach *)in;
	x->reverse = 0;
	x->extra_attrs = 0;
	T(x)->style = look(x);
}

static int mach_key(struct vt_interp *in, int k, int mods, dmc_buf *out)
{
	struct mach *x = (struct mach *)in;
	int shift = mods & M_SHIFT;
	switch (x->kind) {
	case MK_PETSCII:
		return pet_key(x, k, shift, out);
	case MK_ATASCII:
		return atari_key(k, shift, out);
	case MK_VIEWDATA:
		if (k == DMC_KEY_ENTER) {
			put_byte(out, 0x5f);
			return 1;
		}
		if (k == DMC_KEY_BACKSPACE) {
			put_byte(out, 0x08);
			return 1;
		}
		return 0;
	case MK_MINITEL:
		if (x->teleinfo)
			return 0;
		if (k == DMC_KEY_ENTER) {
			put_byte(out, 0x13);
			put_byte(out, 0x41);
			return 1;
		}
		if (k == DMC_KEY_BACKSPACE) {
			put_byte(out, 0x13);
			put_byte(out, 0x47);
			return 1;
		}
		return 0;
	}
	return 0;
}

static int mach_chr(struct vt_interp *in, uint32_t cp, int mods, dmc_buf *out)
{
	struct mach *x = (struct mach *)in;
	int alt = mods & M_ALT, ctrl = mods & M_CTRL, b, k, i;
	switch (x->kind) {
	case MK_PETSCII:
		b = pet_code(x, cp, alt, ctrl);
		if (b >= 0)
			put_byte(out, b);
		return 1;
	case MK_ATASCII:
		b = atari_code(x, cp, ctrl);
		put_byte(out, alt && !atari_controls(b) ? b | 0x80 : b);
		return 1;
	case MK_VIEWDATA:
		if (cp == '#') {
			put_byte(out, 0x5f);
			return 1;
		}
		if (cp == 0xa3) {
			put_byte(out, 0x23);
			return 1;
		}
		if (cp >= 0x20 && cp <= 0x7e) {
			put_byte(out, (int)cp);
			return 1;
		}
		return 0;
	case MK_MINITEL:
		if (x->teleinfo || ctrl || alt || cp < 0x80)
			return 0;
		for (k = 0; k < 4; k++)
			for (i = 0; i < 10; i++)
				if (typed_letters[k][i] == cp) {
					put_byte(out, 0x19);
					put_byte(out, typed_accent[k]);
					put_byte(out, accented_base[i]);
					return 1;
				}
		if (cp == 0xe7) {
			put_byte(out, 0x19);
			put_byte(out, 0x4b);
			put_byte(out, 'c');
			return 1;
		}
		for (k = 0; k < N_G2; k++)
			if (g2[k][1] == cp) {
				put_byte(out, 0x19);
				put_byte(out, (int)g2[k][0]);
				return 1;
			}
		return 0;
	}
	return 0;
}

static void mach_flush(struct vt_interp *in)
{
	struct mach *x = (struct mach *)in;
	if (x->teleinfo && x->teleinfo->ops->flush)
		x->teleinfo->ops->flush(x->teleinfo);
}

static void mach_detach(struct vt_interp *in)
{
	struct mach *x = (struct mach *)in;
	dmc_vt *t = T(x);
	vt_release_machine(t);
	if (x->kind == MK_VIEWDATA || x->kind == MK_MINITEL)
		t->soft_glyphs = NULL;
}

static void mach_destroy(struct vt_interp *in)
{
	struct mach *x = (struct mach *)in;
	if (x->teleinfo)
		interp_free(x->teleinfo);
	free(x);
}

static const struct vt_interp_ops mach_ops = {
	process, mach_reset, mach_soft_reset, NULL, NULL, mach_key, mach_chr,
	NULL, mach_flush, mach_detach, mach_destroy
};

static struct vt_interp *mach_new(dmc_vt *t, const struct vt_profile *p,
				  int kind)
{
	struct mach *x = (struct mach *)calloc(1, sizeof(*x));
	if (!x)
		return NULL;
	x->in.ops = &mach_ops;
	x->in.t = t;
	x->in.reads_octets = 1;
	x->m = p;
	x->kind = kind;
	x->color = p->default_color;
	reset_tabs(x, 1);
	attach(x);
	if (kind == MK_VIEWDATA || kind == MK_MINITEL) {
		t->soft_glyphs = &vt_font_mosaic;
		vd_blank(x);
	}
	if (kind == MK_MINITEL) {
		mt_attributes(x);
		at(t, 0, 1);
	}
	return &x->in;
}

struct vt_interp *petscii_new(dmc_vt *t, const struct vt_profile *p)
{
	return mach_new(t, p, MK_PETSCII);
}

struct vt_interp *atascii_new(dmc_vt *t, const struct vt_profile *p)
{
	return mach_new(t, p, MK_ATASCII);
}

struct vt_interp *viewdata_new(dmc_vt *t, const struct vt_profile *p)
{
	return mach_new(t, p, MK_VIEWDATA);
}

struct vt_interp *minitel_new(dmc_vt *t, const struct vt_profile *p)
{
	return mach_new(t, p, MK_MINITEL);
}
