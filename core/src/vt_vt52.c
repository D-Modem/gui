/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * vt_vt52.c - the DEC VT52's escape sequences (the app's Vt52Interpreter):
 * one character after ESC (ESC Y and two more for the cursor's place),
 * no parameters, no attributes.  A VT52 profile has it as its
 * interpreter; a VT100 and later run one in VT52 mode until ESC < hands
 * back.  And an Atari ST's TOS VT52 (AtariStInterpreter), with the ST's
 * own additions and its character set.
 */
#include <stdlib.h>
#include <string.h>

#include "vtint.h"

enum { V_GROUND, V_ESC, V_ROW, V_COL, V_ARG };

struct vt52 {
	struct vt_interp in;
	int graphics_set;
	const char *identity;
	void (*on_ansi)(void *);
	void *ctx;
	int state, arg_for, graphics, row;
	struct vt_printer printer;
	/* an Atari ST's */
	const struct vt_profile *st;
	int fg, bg, reverse;
};

#define T(v) ((v)->in.t)

static void control(struct vt52 *v, int32_t cp)
{
	dmc_vt *t = T(v);
	switch (cp) {
	case 0x07: vt_bell(t); break;
	case 0x08: vt_backspace(t); break;
	case 0x09: vt_tab(t, 1); break;
	case 0x0a:
		if (v->printer.autop)
			printer_line(&v->printer, t->cy);
		vt_index(t);
		break;
	case 0x0d: vt_carriage_return(t); break;
	}
}

static void st_restyle(struct vt52 *v)
{
	T(v)->style = st_of(v->fg, v->bg, v->reverse ? A_INVERSE : 0);
}

static void esc(struct vt52 *v, int32_t cp)
{
	dmc_vt *t = T(v);
	if (v->st) {
		switch (cp) {
		case 'E':
			vt_erase_display(t, 2, 0);
			vt_move_cursor(t, 0, 0, 0);
			return;
		case 'd': vt_erase_display(t, 1, 0); return;
		case 'e': t->cursor_visible = 1; return;
		case 'f': t->cursor_visible = 0; return;
		case 'j': vt_save_cursor(t); return;
		case 'k': vt_restore_cursor(t); return;
		case 'l':
			vt_erase_line(t, 2, 0);
			vt_carriage_return(t);
			return;
		case 'o': vt_erase_line(t, 1, 0); return;
		case 'p': v->reverse = 1; st_restyle(v); return;
		case 'q': v->reverse = 0; st_restyle(v); return;
		case 'v': t->autowrap = 1; return;
		case 'w': t->autowrap = 0; return;
		case 'L':
			vt_insert_lines(t, 1);
			vt_carriage_return(t);
			return;
		case 'M':
			vt_delete_lines(t, 1);
			vt_carriage_return(t);
			return;
		case 'b': case 'c':
			v->arg_for = cp;
			v->state = V_ARG;
			return;
		}
	}
	switch (cp) {
	case 'A': vt_move_cursor(t, t->cx, t->cy - 1, 0); break;
	case 'B': vt_move_cursor(t, t->cx, t->cy + 1, 0); break;
	case 'C': vt_move_cursor(t, t->cx + 1, t->cy, 0); break;
	case 'D': vt_move_cursor(t, t->cx - 1, t->cy, 0); break;
	case 'F': v->graphics = 1; break;
	case 'G': v->graphics = 0; break;
	case 'H': vt_move_cursor(t, 0, 0, 0); break;
	case 'I': vt_reverse_index(t); break;
	case 'J': vt_erase_display(t, 0, 0); break;
	case 'K': vt_erase_line(t, 0, 0); break;
	case 'Y': v->state = V_ROW; break;
	case 'Z': vt_reply(t, v->identity); break;
	case '=': t->app_keypad = 1; break;
	case '>': t->app_keypad = 0; break;
	case '<': if (v->on_ansi) v->on_ansi(v->ctx); break;
	case '[': case '\\': break;
	case '^': v->printer.autop = 1; break;
	case '_': v->printer.autop = 0; break;
	case 'W': printer_start(&v->printer); break;
	case 'V': printer_line(&v->printer, t->cy); break;
	case ']': printer_screen(&v->printer); break;
	default:
		if (cp == 0x1b)
			v->state = V_ESC;
		else if (cp < 0x20)
			control(v, cp);
	}
}

static void process(struct vt_interp *in, int32_t cp)
{
	struct vt52 *v = (struct vt52 *)in;
	dmc_vt *t = T(v);
	int y, pen;
	if (v->printer.controller) {
		printer_feed(&v->printer, cp);
		return;
	}
	switch (v->state) {
	case V_GROUND:
		if (cp == 0x1b)
			v->state = V_ESC;
		else if (cp < 0x20)
			control(v, cp);
		else if (cp == 0x7f)
			;
		else if (v->graphics)
			vt_print(t, (int32_t)gset_map(v->graphics_set, (uint32_t)cp));
		else
			vt_print(t, v->st ? (int32_t)atari_st_char(cp) : cp);
		break;
	case V_ESC:
		v->state = V_GROUND;
		esc(v, cp);
		break;
	case V_ROW:
		v->row = cp - 0x20;
		v->state = V_COL;
		break;
	case V_ARG:
		v->state = V_GROUND;
		pen = cp & (v->st->ncolors - 1);
		if (v->arg_for == 'b')
			v->fg = pen;
		else
			v->bg = pen;
		st_restyle(v);
		break;
	case V_COL:
		v->state = V_GROUND;
		/* (a row off the screen leaves the cursor's) */
		y = v->row >= 0 && v->row < t->rows ? v->row : t->cy;
		vt_move_cursor(t, cp - 0x20, y, 0);
		break;
	}
}

static void st_attach(struct vt52 *v)
{
	dmc_vt *t = T(v);
	const struct vt_profile *m = v->st;
	vt_use_machine(t, m->columns, m->rows, m->colors, m->ncolors,
		       m->default_color, m->background);
	t->autowrap = 1;
	t->cursor_visible = 1;
	st_restyle(v);
}

static void reset(struct vt_interp *in)
{
	struct vt52 *v = (struct vt52 *)in;
	v->state = V_GROUND;
	v->graphics = 0;
	printer_reset(&v->printer);
	T(v)->vt52_keys = 1;
	if (v->st) {
		v->fg = v->st->default_color;
		v->bg = v->st->background;
		v->reverse = 0;
		st_attach(v);
	}
}

static void detach(struct vt_interp *in)
{
	struct vt52 *v = (struct vt52 *)in;
	if (v->st)
		vt_release_machine(T(v));
	T(v)->vt52_keys = 0;
}

static void destroy(struct vt_interp *in)
{
	struct vt52 *v = (struct vt52 *)in;
	printer_free(&v->printer);
	free(v);
}

static const struct vt_interp_ops vt52_ops = {
	process, reset, NULL, NULL, NULL, NULL, NULL, NULL, NULL, detach,
	destroy
};

struct vt_interp *vt52_new(dmc_vt *t, int graphics_set, const char *identity,
			   void (*on_ansi)(void *), void *ctx)
{
	struct vt52 *v = (struct vt52 *)calloc(1, sizeof(*v));
	if (!v)
		return NULL;
	v->in.ops = &vt52_ops;
	v->in.t = t;
	v->graphics_set = graphics_set;
	v->identity = identity;
	v->on_ansi = on_ansi;
	v->ctx = ctx;
	printer_init(&v->printer, t, "\x1bX", NULL);
	t->vt52_keys = 1;
	return &v->in;
}

struct vt_interp *atari_st_new(dmc_vt *t, const struct vt_profile *p)
{
	struct vt_interp *in = vt52_new(t, G_VT52_GRAPHICS, "\x1b/K", NULL, NULL);
	struct vt52 *v = (struct vt52 *)in;
	if (!v)
		return NULL;
	in->reads_octets = 1;
	v->st = p;
	v->fg = p->default_color;
	v->bg = p->background;
	st_attach(v);
	return in;
}
