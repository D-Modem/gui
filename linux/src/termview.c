/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * termview.c - a terminal widget: a screen of libdmodemctl's dmc_vt,
 *              drawn with Cairo and Pango, its keys handed to whoever
 *              owns it.  The Terminal page's and the Remote page's.
 *
 * Every kind of terminal the engine is (xterm, the VTs, ANSI-BBS, the
 * Commodores and Ataris, Viewdata, the Minitel, the Tektronix) is drawn
 * here: a cell's background, its character in the monospace font or, where
 * the profile has its machine's font, the glyph's bitmap (each row's
 * glyphs put in an image at the font's own size and stretched over the
 * row, so a C64's 8x8 stays sharp at any size), its lines, the pictures
 * (sixel, ReGIS, RIPscrip's, the Tektronix's) over the text, and the
 * cursor.  A machine with a screen of its own size (a C64's 40x25) has its
 * cells stretched to fill the widget; DEC's double width and height lines
 * are drawn scaled.
 */
#include <math.h>
#include <string.h>

#include "app.h"

#define MAX_COLS 400

typedef struct TermView {
	GtkWidget *box, *area, *bar;
	GtkAdjustment *adj;
	dmc_vt *vt;
	TermTypedFn fn;
	void *ctx;
	PangoFontDescription *font[4];  /* regular, bold, italic, both */
	int fcw, fch, fascent;          /* the font's cell */
	int cw, ch, ox, oy;             /* a cell as drawn, the screen's place */
	int status_rows;
	int scroll;                     /* 0 the screen, -n back in history */
	gboolean focused, blink_off, has_blink, adjusting;
	int buttons;
	guint blink_tick;
	GtkIMContext *im;
} TermView;

static TermView *of(GtkWidget *w)
{
	return g_object_get_data(G_OBJECT(w), "termview");
}

static void rgb(cairo_t *cr, uint32_t c)
{
	cairo_set_source_rgb(cr, ((c >> 16) & 255) / 255.0,
			     ((c >> 8) & 255) / 255.0, (c & 255) / 255.0);
}

/*
 *    the screen's size
 */

static void measure(TermView *t)
{
	PangoContext *pc = gtk_widget_get_pango_context(t->area);
	PangoFontMetrics *m = pango_context_get_metrics(pc, t->font[0], NULL);
	int w = pango_font_metrics_get_approximate_digit_width(m);
	int a = pango_font_metrics_get_ascent(m), d = pango_font_metrics_get_descent(m);
	t->fcw = (w + PANGO_SCALE - 1) / PANGO_SCALE;
	t->fch = (a + d + PANGO_SCALE - 1) / PANGO_SCALE;
	t->fascent = a / PANGO_SCALE;
	if (t->fcw < 4)
		t->fcw = 8;
	if (t->fch < 6)
		t->fch = 16;
	pango_font_metrics_unref(m);
}

static void update_bar(TermView *t)
{
	int hist = dmc_vt_history(t->vt), rows = dmc_vt_rows(t->vt);
	t->adjusting = TRUE;
	gtk_adjustment_configure(t->adj, hist + t->scroll, 0, hist + rows, 1,
				 rows, rows);
	t->adjusting = FALSE;
}

/* the columns and rows that fit, or the machine's own with its cells
   stretched to fill the widget */
static void layout(TermView *t)
{
	int W = gtk_widget_get_width(t->area), H = gtk_widget_get_height(t->area);
	if (W <= 0 || H <= 0)
		return;
	int rc = dmc_vt_requested_cols(t->vt), rr = dmc_vt_requested_rows(t->vt);
	const char *st = dmc_vt_status_line(t->vt);
	t->status_rows = st && *st ? 1 : 0;
	int gw, gh, cw = t->fcw, ch = t->fch, cols, rows;
	gboolean glyphs = dmc_vt_glyph_cell(t->vt, &gw, &gh) != 0;
	if (glyphs && rc > 0 && rr > 0) {
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
	cols = CLAMP(cols, 10, MAX_COLS);
	rows = CLAMP(rows, 3, 200);
	t->cw = cw;
	t->ch = ch;
	t->ox = rc > 0 ? MAX(0, (W - cols * cw) / 2) : 0;
	t->oy = rr > 0 ? MAX(0, (H - (rows + t->status_rows) * ch) / 2) : 0;
	dmc_vt_set_cell_pixels(t->vt, cw, ch);
	if (cols != dmc_vt_cols(t->vt) || rows != dmc_vt_rows(t->vt)) {
		dmc_vt_resize(t->vt, cols, rows);
		update_bar(t);
	}
}

/*
 *    drawing
 */

/* a row's machine-font glyphs at the font's size, to be stretched */
static void draw_glyphs(TermView *t, cairo_t *cr, const dmc_cell *cells,
			int ncols)
{
	int gw, gh;
	if (!dmc_vt_glyph_cell(t->vt, &gw, &gh)) {
		/* (soft characters only: their own size) */
		gw = 8;
		gh = 16;
		for (int x = 0; x < ncols; x++)
			if ((cells[x].flags & DMC_CELL_GLYPH) &&
			    dmc_vt_glyph(t->vt, cells[x].ch, &gw, &gh))
				break;
	}
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
							ncols * gw, gh);
	if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(s);
		return;
	}
	cairo_surface_flush(s);
	unsigned char *data = cairo_image_surface_get_data(s);
	int stride = cairo_image_surface_get_stride(s);
	for (int x = 0; x < ncols; x++) {
		const dmc_cell *c = &cells[x];
		int w, h;
		if (!(c->flags & DMC_CELL_GLYPH) || (c->flags & DMC_CELL_INVISIBLE) ||
		    ((c->flags & DMC_CELL_BLINK) && t->blink_off) ||
		    !dmc_vt_glyph(t->vt, c->ch, &w, &h) || w <= 0 || h <= 0)
			continue;
		int span = (c->flags & DMC_CELL_WIDE) && x + 1 < ncols ? 2 : 1;
		uint32_t fg = 0xff000000u | (c->fg & 0xffffff);
		for (int y = 0; y < gh; y++) {
			uint32_t bits = dmc_vt_glyph_row(t->vt, c->ch, y * h / gh);
			if (!bits)
				continue;
			uint32_t *p = (uint32_t *)(data + (size_t)y * stride) + x * gw;
			for (int i = 0; i < gw * span; i++) {
				int gx = i * w / (gw * span);
				if ((bits >> (w - 1 - gx)) & 1)
					p[i] = fg;
			}
		}
	}
	cairo_surface_mark_dirty(s);
	cairo_save(cr);
	cairo_scale(cr, (double)t->cw / gw, (double)t->ch / gh);
	cairo_set_source_surface(cr, s, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
	cairo_paint(cr);
	cairo_restore(cr);
	cairo_surface_destroy(s);
}

/* the pictures' surfaces made for this drawing, by id */
static cairo_surface_t *picture(GHashTable *made, const uint32_t *px,
				const dmc_image *im)
{
	cairo_surface_t *s = g_hash_table_lookup(made, GUINT_TO_POINTER(im->id));
	if (s)
		return s;
	s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, im->width, im->height);
	if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS)
		return NULL;
	cairo_surface_flush(s);
	unsigned char *data = cairo_image_surface_get_data(s);
	int stride = cairo_image_surface_get_stride(s);
	for (int y = 0; y < im->height; y++) {
		uint32_t *d = (uint32_t *)(data + (size_t)y * stride);
		const uint32_t *p = px + (size_t)y * im->width;
		for (int x = 0; x < im->width; x++) {
			uint32_t v = p[x], a = v >> 24;
			/* (Cairo's are premultiplied) */
			d[x] = (a << 24) | ((((v >> 16) & 255) * a / 255) << 16) |
				((((v >> 8) & 255) * a / 255) << 8) | ((v & 255) * a / 255);
		}
	}
	cairo_surface_mark_dirty(s);
	g_hash_table_insert(made, GUINT_TO_POINTER(im->id), s);
	return s;
}

/* view row vrow at (0, 0): ncols cells */
static void draw_row(TermView *t, cairo_t *cr, PangoLayout *lay, int vrow,
		     int ncols, gboolean cursor_here, GHashTable *pics)
{
	dmc_vt *vt = t->vt;
	int cw = t->cw, ch = t->ch;
	dmc_cell cells[MAX_COLS];
	gboolean any_glyph = FALSE, any_image = FALSE;
	ncols = MIN(ncols, MAX_COLS);
	for (int x = 0; x < ncols; x++) {
		dmc_vt_cell(vt, vrow, x, &cells[x]);
		if (cells[x].flags & DMC_CELL_BLINK)
			t->has_blink = TRUE;
		if (cells[x].flags & DMC_CELL_GLYPH)
			any_glyph = TRUE;
		if (cells[x].flags & DMC_CELL_IMAGE)
			any_image = TRUE;
	}
	/* the backgrounds, in runs */
	for (int x = 0; x < ncols;) {
		int n = 1;
		while (x + n < ncols && cells[x + n].bg == cells[x].bg)
			n++;
		rgb(cr, cells[x].bg);
		cairo_rectangle(cr, x * cw, 0, n * cw, ch);
		cairo_fill(cr);
		x += n;
	}
	/* the text, in the font (stretched when the cells are) */
	double sx = (double)cw / t->fcw, sy = (double)ch / t->fch;
	int last_font = -1;
	for (int x = 0; x < ncols; x++) {
		const dmc_cell *c = &cells[x];
		int f = c->flags;
		if ((f & (DMC_CELL_GLYPH | DMC_CELL_INVISIBLE | DMC_CELL_WIDE_TAIL)) ||
		    ((f & DMC_CELL_BLINK) && t->blink_off) ||
		    (c->ch == ' ' && !(f & DMC_CELL_COMBINING)) || c->ch == 0)
			continue;
		int fi = ((f & DMC_CELL_BOLD) ? 1 : 0) | ((f & DMC_CELL_ITALIC) ? 2 : 0);
		if (fi != last_font) {
			pango_layout_set_font_description(lay, t->font[fi]);
			last_font = fi;
		}
		if (f & DMC_CELL_COMBINING) {
			pango_layout_set_text(lay, dmc_vt_cell_text(vt, vrow, x), -1);
		} else {
			char u[8];
			int n = g_unichar_to_utf8(c->ch, u);
			pango_layout_set_text(lay, u, n);
		}
		rgb(cr, c->fg);
		cairo_save(cr);
		cairo_translate(cr, x * cw, 0);
		if (sx != 1 || sy != 1)
			cairo_scale(cr, sx, sy);
		cairo_move_to(cr, 0, 0);
		pango_cairo_show_layout(cr, lay);
		cairo_restore(cr);
	}
	if (any_glyph)
		draw_glyphs(t, cr, cells, ncols);
	/* the lines */
	double th = MAX(1, ch / 16);
	for (int x = 0; x < ncols; x++) {
		const dmc_cell *c = &cells[x];
		int f = c->flags, l = x * cw;
		if (f & DMC_CELL_INVISIBLE)
			continue;
		rgb(cr, c->fg);
		if (f & (DMC_CELL_UNDERLINE | DMC_CELL_LINK))
			cairo_rectangle(cr, l, ch - 2 * th, cw, th);
		if (f & DMC_CELL_DOUBLE_UNDERLINE) {
			cairo_rectangle(cr, l, ch - th, cw, th);
			cairo_rectangle(cr, l, ch - 3 * th, cw, th);
		}
		if (f & DMC_CELL_STRIKE)
			cairo_rectangle(cr, l, ch / 2, cw, th);
		if (f & DMC_CELL_OVERLINE)
			cairo_rectangle(cr, l, 0, cw, th);
		cairo_fill(cr);
		if (f & DMC_CELL_CURLY_UNDERLINE) {
			cairo_set_line_width(cr, th);
			cairo_move_to(cr, l, ch - 2 * th);
			for (int i = 0; i < 4; i++)
				cairo_line_to(cr, l + (i + 1) * cw / 4.0,
					      ch - (i % 2 ? 2 : 1) * th - th / 2);
			cairo_stroke(cr);
		}
	}
	/* the pictures over the text */
	if (any_image)
		for (int x = 0; x < ncols; x++) {
			dmc_image im;
			const uint32_t *px;
			if (!(cells[x].flags & DMC_CELL_IMAGE) ||
			    !(px = dmc_vt_cell_image(vt, vrow, x, &im)) ||
			    im.cell_width <= 0 || im.cell_height <= 0)
				continue;
			cairo_surface_t *s = picture(pics, px, &im);
			if (!s)
				continue;
			cairo_save(cr);
			cairo_rectangle(cr, x * cw, 0, cw, ch);
			cairo_clip(cr);
			cairo_translate(cr, x * cw, 0);
			cairo_scale(cr, (double)cw / im.cell_width,
				    (double)ch / im.cell_height);
			cairo_set_source_surface(cr, s, -im.tx * im.cell_width,
						 -im.ty * im.cell_height);
			cairo_pattern_set_filter(cairo_get_source(cr),
						 CAIRO_FILTER_NEAREST);
			cairo_paint(cr);
			cairo_restore(cr);
		}
	/* the cursor */
	if (cursor_here) {
		int cx = dmc_vt_cursor_col(vt);
		if (cx >= 0 && cx < ncols) {
			int w = (cells[cx].flags & DMC_CELL_WIDE) ? 2 * cw : cw;
			int style = dmc_vt_cursor_style(vt);
			rgb(cr, dmc_vt_cursor_color(vt));
			if (!t->focused) {
				cairo_set_line_width(cr, 1);
				cairo_rectangle(cr, cx * cw + 0.5, 0.5, w - 1, ch - 1);
				cairo_stroke(cr);
			} else if (style == 3 || style == 4) {
				cairo_rectangle(cr, cx * cw, ch - MAX(2, ch / 8), w,
						MAX(2, ch / 8));
				cairo_fill(cr);
			} else if (style == 5 || style == 6) {
				cairo_rectangle(cr, cx * cw, 0, MAX(2, cw / 8), ch);
				cairo_fill(cr);
			} else {
				cairo_save(cr);
				cairo_rectangle(cr, cx * cw, 0, w, ch);
				cairo_set_operator(cr, CAIRO_OPERATOR_DIFFERENCE);
				cairo_fill(cr);
				cairo_restore(cr);
			}
		}
	}
}

static void draw(GtkDrawingArea *area, cairo_t *cr, int W, int H, gpointer data)
{
	TermView *t = data;
	dmc_vt *vt = t->vt;
	(void)area;
	rgb(cr, dmc_vt_background(vt));
	cairo_rectangle(cr, 0, 0, W, H);
	cairo_fill(cr);
	int rows = dmc_vt_rows(vt), cols = dmc_vt_cols(vt);
	int cw = t->cw, ch = t->ch;
	PangoLayout *lay = gtk_widget_create_pango_layout(t->area, NULL);
	GHashTable *pics = g_hash_table_new_full(NULL, NULL, NULL,
		(GDestroyNotify)cairo_surface_destroy);
	t->has_blink = FALSE;
	for (int y = 0; y < rows; y++) {
		int vrow = y + t->scroll;
		int size = dmc_vt_line_size(vt, vrow);
		gboolean cur = t->scroll == 0 && dmc_vt_cursor_visible(vt) &&
			dmc_vt_cursor_row(vt) == y;
		cairo_save(cr);
		cairo_translate(cr, t->ox, t->oy + y * ch);
		cairo_rectangle(cr, 0, 0, cols * cw, ch);
		cairo_clip(cr);
		if (size == DMC_LINE_SINGLE) {
			draw_row(t, cr, lay, vrow, cols, cur, pics);
		} else {
			if (size == DMC_LINE_DOUBLE_WIDTH) {
				cairo_scale(cr, 2, 1);
			} else {
				if (size == DMC_LINE_DOUBLE_BOTTOM)
					cairo_translate(cr, 0, -ch);
				cairo_scale(cr, 2, 2);
			}
			draw_row(t, cr, lay, vrow, (cols + 1) / 2, cur, pics);
		}
		cairo_restore(cr);
	}
	if (t->status_rows) {
		int y = t->oy + rows * ch;
		rgb(cr, dmc_vt_foreground(vt));
		cairo_rectangle(cr, t->ox, y, cols * cw, ch);
		cairo_fill(cr);
		pango_layout_set_font_description(lay, t->font[0]);
		pango_layout_set_text(lay, dmc_vt_status_line(vt), -1);
		rgb(cr, dmc_vt_background(vt));
		cairo_move_to(cr, t->ox, y);
		pango_cairo_show_layout(cr, lay);
	}
	g_hash_table_destroy(pics);
	g_object_unref(lay);
}

/*
 *    what the user does
 */

static void send_text(TermView *t, const char *s, int n)
{
	if (t->fn && n > 0)
		t->fn(s, n, t->ctx);
}

static void to_bottom(TermView *t)
{
	if (t->scroll) {
		t->scroll = 0;
		update_bar(t);
		gtk_widget_queue_draw(t->area);
	}
}

static int mods_of(GdkModifierType s)
{
	int m = 0;
	if (s & GDK_SHIFT_MASK)
		m |= DMC_MOD_SHIFT;
	if (s & GDK_CONTROL_MASK)
		m |= DMC_MOD_CTRL;
	if (s & GDK_ALT_MASK)
		m |= DMC_MOD_ALT;
	return m;
}

static void send_key(TermView *t, int key, int mods)
{
	char b[64];
	to_bottom(t);
	send_text(t, b, dmc_vt_key_mods(t->vt, key, mods, b, sizeof(b)));
}

static void send_cp(TermView *t, gunichar c, int mods)
{
	char b[64];
	send_text(t, b, dmc_vt_char_bytes(t->vt, c, mods, b, sizeof(b)));
}

static void send_utf8(TermView *t, const char *s, gboolean paste)
{
	gboolean bracket = paste && dmc_vt_bracketed_paste(t->vt);
	if (bracket)
		send_text(t, "\x1b[200~", 6);
	for (const char *p = s; *p; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);
		if (c == '\n' && p > s && p[-1] == '\r')
			continue;
		send_cp(t, c == '\n' ? '\r' : c, 0);
	}
	if (bracket)
		send_text(t, "\x1b[201~", 6);
}

static void committed(GtkIMContext *im, const char *s, gpointer data)
{
	TermView *t = data;
	(void)im;
	to_bottom(t);
	send_utf8(t, s, FALSE);
}

static void pasted(GObject *o, GAsyncResult *res, gpointer data)
{
	TermView *t = data;
	char *s = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(o), res, NULL);
	if (s) {
		to_bottom(t);
		send_utf8(t, s, TRUE);
		g_free(s);
	}
	g_object_unref(t->box);
}

static void paste(TermView *t, gboolean primary)
{
	GdkClipboard *c = primary ? gtk_widget_get_primary_clipboard(t->area) :
		gtk_widget_get_clipboard(t->area);
	g_object_ref(t->box);
	gdk_clipboard_read_text_async(c, NULL, pasted, t);
}

static int key_of(guint kv)
{
	switch (kv) {
	case GDK_KEY_Up: case GDK_KEY_KP_Up: return DMC_KEY_UP;
	case GDK_KEY_Down: case GDK_KEY_KP_Down: return DMC_KEY_DOWN;
	case GDK_KEY_Left: case GDK_KEY_KP_Left: return DMC_KEY_LEFT;
	case GDK_KEY_Right: case GDK_KEY_KP_Right: return DMC_KEY_RIGHT;
	case GDK_KEY_Home: case GDK_KEY_KP_Home: return DMC_KEY_HOME;
	case GDK_KEY_End: case GDK_KEY_KP_End: return DMC_KEY_END;
	case GDK_KEY_Page_Up: case GDK_KEY_KP_Page_Up: return DMC_KEY_PGUP;
	case GDK_KEY_Page_Down: case GDK_KEY_KP_Page_Down: return DMC_KEY_PGDN;
	case GDK_KEY_Insert: case GDK_KEY_KP_Insert: return DMC_KEY_INSERT;
	case GDK_KEY_Delete: case GDK_KEY_KP_Delete: return DMC_KEY_DELETE;
	case GDK_KEY_ISO_Left_Tab: return DMC_KEY_BACKTAB;
	case GDK_KEY_Tab: return DMC_KEY_TAB;
	case GDK_KEY_Return: return DMC_KEY_ENTER;
	case GDK_KEY_KP_Enter: return DMC_KEY_KP_ENTER;
	case GDK_KEY_BackSpace: return DMC_KEY_BACKSPACE;
	case GDK_KEY_Escape: return DMC_KEY_ESCAPE;
	case GDK_KEY_F11: return DMC_KEY_F11;
	case GDK_KEY_F12: return DMC_KEY_F12;
	case GDK_KEY_Find: return DMC_KEY_FIND;
	case GDK_KEY_Select: return DMC_KEY_SELECT;
	}
	if (kv >= GDK_KEY_F1 && kv <= GDK_KEY_F10)
		return DMC_KEY_F1 + (int)(kv - GDK_KEY_F1);
	if (kv >= GDK_KEY_F13 && kv <= GDK_KEY_F20)
		return DMC_KEY_F13 + (int)(kv - GDK_KEY_F13);
	return 0;
}

static gboolean key_pressed(GtkEventControllerKey *k, guint kv, guint code,
			    GdkModifierType state, gpointer data)
{
	TermView *t = data;
	int mods = mods_of(state);
	(void)k;
	(void)code;
	if (mods == DMC_MOD_SHIFT) {
		if (kv == GDK_KEY_Page_Up || kv == GDK_KEY_Page_Down) {
			double v = gtk_adjustment_get_value(t->adj) +
				(kv == GDK_KEY_Page_Up ? -1 : 1) *
				gtk_adjustment_get_page_size(t->adj);
			gtk_adjustment_set_value(t->adj, v);
			return TRUE;
		}
		if (kv == GDK_KEY_Insert) {
			paste(t, FALSE);
			return TRUE;
		}
	}
	if ((mods & DMC_MOD_CTRL) && (mods & DMC_MOD_SHIFT) &&
	    (kv == GDK_KEY_V || kv == GDK_KEY_v)) {
		paste(t, FALSE);
		return TRUE;
	}
	int key = key_of(kv);
	if (key) {
		send_key(t, key, mods);
		return TRUE;
	}
	/* Ctrl or Alt with a character: the terminal's controls, ESC and it */
	gunichar c = gdk_keyval_to_unicode(kv);
	if (c && (mods & (DMC_MOD_CTRL | DMC_MOD_ALT))) {
		to_bottom(t);
		send_cp(t, c, mods & (DMC_MOD_CTRL | DMC_MOD_ALT));
		return TRUE;
	}
	return FALSE;
}

/* the cell under a point */
static void cell_at(TermView *t, double x, double y, int *col, int *row)
{
	*col = MAX(0, (int)((x - t->ox) / t->cw));
	*row = MAX(0, (int)((y - t->oy) / t->ch));
}

static gboolean mouse_report(TermView *t, int button, double x, double y,
			     int press, int motion, GdkModifierType state)
{
	if (!dmc_vt_mouse_wanted(t->vt))
		return FALSE;
	int col, row;
	char b[64];
	cell_at(t, x, y, &col, &row);
	send_text(t, b, dmc_vt_mouse(t->vt, button, col, row, press, motion,
				     mods_of(state), b, sizeof(b)));
	return TRUE;
}

static int button_of(GtkGestureClick *g)
{
	guint b = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
	return b == 1 ? 0 : b == 2 ? 1 : 2;
}

static void pressed(GtkGestureClick *g, int n, double x, double y, gpointer data)
{
	TermView *t = data;
	int b = button_of(g);
	GdkModifierType st = gtk_event_controller_get_current_event_state(
		GTK_EVENT_CONTROLLER(g));
	(void)n;
	gtk_widget_grab_focus(t->area);
	t->buttons |= 1 << b;
	if (mouse_report(t, b, x, y, 1, 0, st))
		return;
	if (b == 0) {
		/* a RIPscrip button's */
		char s[256];
		double fx = (x - t->ox) / (dmc_vt_cols(t->vt) * t->cw);
		double fy = (y - t->oy) / (dmc_vt_rows(t->vt) * t->ch);
		int k = dmc_vt_tap(t->vt, fx, fy, s, sizeof(s));
		if (k > 0) {
			to_bottom(t);
			send_text(t, s, k);
		}
	} else {
		paste(t, b == 1);
	}
}

static void released(GtkGestureClick *g, int n, double x, double y,
		     gpointer data)
{
	TermView *t = data;
	int b = button_of(g);
	(void)n;
	t->buttons &= ~(1 << b);
	mouse_report(t, b, x, y, 0, 0, gtk_event_controller_get_current_event_state(
			     GTK_EVENT_CONTROLLER(g)));
}

static void moved(GtkEventControllerMotion *m, double x, double y, gpointer data)
{
	TermView *t = data;
	int b = t->buttons & 1 ? 0 : t->buttons & 2 ? 1 : t->buttons & 4 ? 2 : 3;
	mouse_report(t, b, x, y, 1, 1, gtk_event_controller_get_current_event_state(
			     GTK_EVENT_CONTROLLER(m)));
}

static gboolean scrolled(GtkEventControllerScroll *s, double dx, double dy,
			 gpointer data)
{
	TermView *t = data;
	(void)dx;
	if (dmc_vt_mouse_wanted(t->vt)) {
		double x = 0, y = 0;
		GdkEvent *e = gtk_event_controller_get_current_event(
			GTK_EVENT_CONTROLLER(s));
		if (e)
			gdk_event_get_position(e, &x, &y);
		mouse_report(t, dy < 0 ? 64 : 65, x, y, 1, 0, 0);
		return TRUE;
	}
	gtk_adjustment_set_value(t->adj, gtk_adjustment_get_value(t->adj) +
				 dy * 3);
	return TRUE;
}

static void focus_changed(TermView *t, gboolean in)
{
	char b[16];
	t->focused = in;
	if (in)
		gtk_im_context_focus_in(t->im);
	else
		gtk_im_context_focus_out(t->im);
	send_text(t, b, dmc_vt_focus(t->vt, in, b, sizeof(b)));
	gtk_widget_queue_draw(t->area);
}

static void focus_in(GtkEventControllerFocus *f, gpointer data)
{
	(void)f;
	focus_changed(data, TRUE);
}

static void focus_out(GtkEventControllerFocus *f, gpointer data)
{
	(void)f;
	focus_changed(data, FALSE);
}

static void scrolled_bar(GtkAdjustment *adj, gpointer data)
{
	TermView *t = data;
	if (t->adjusting)
		return;
	int hist = dmc_vt_history(t->vt);
	int pos = CLAMP((int)gtk_adjustment_get_value(adj), 0, hist);
	t->scroll = pos - hist;
	gtk_widget_queue_draw(t->area);
}

static void resized(GtkDrawingArea *a, int w, int h, gpointer data)
{
	(void)a;
	(void)w;
	(void)h;
	layout(data);
}

static gboolean blink(gpointer data)
{
	TermView *t = data;
	t->blink_off = !t->blink_off;
	if (t->has_blink)
		gtk_widget_queue_draw(t->area);
	return G_SOURCE_CONTINUE;
}

static void destroyed(gpointer data)
{
	TermView *t = data;
	if (t->blink_tick)
		g_source_remove(t->blink_tick);
	for (int i = 0; i < 4; i++)
		pango_font_description_free(t->font[i]);
	g_object_unref(t->im);
	dmc_vt_free(t->vt);
	g_free(t);
}

GtkWidget *termview_new(TermTypedFn fn, void *ctx)
{
	TermView *t = g_new0(TermView, 1);
	t->fn = fn;
	t->ctx = ctx;
	t->vt = dmc_vt_new(80, 24, 5000);
	dmc_vt_set_backspace(t->vt, 0x7f);
	/* the brand's colours */
	dmc_vt_set_theme(t->vt, 0xe8f0ff, 0x0a0f1c, 0xe8f0ff, NULL);
	t->font[0] = pango_font_description_from_string("Monospace 11");
	for (int i = 1; i < 4; i++) {
		t->font[i] = pango_font_description_copy(t->font[0]);
		if (i & 1)
			pango_font_description_set_weight(t->font[i], PANGO_WEIGHT_BOLD);
		if (i & 2)
			pango_font_description_set_style(t->font[i], PANGO_STYLE_ITALIC);
	}
	t->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	t->area = gtk_drawing_area_new();
	gtk_widget_set_hexpand(t->area, TRUE);
	gtk_widget_set_vexpand(t->area, TRUE);
	gtk_widget_set_focusable(t->area, TRUE);
	gtk_widget_set_cursor_from_name(t->area, "text");
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(t->area), draw, t, NULL);
	g_signal_connect(t->area, "resize", G_CALLBACK(resized), t);
	t->adj = gtk_adjustment_new(0, 0, 24, 1, 24, 24);
	g_signal_connect(t->adj, "value-changed", G_CALLBACK(scrolled_bar), t);
	t->bar = gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL, t->adj);
	gtk_box_append(GTK_BOX(t->box), t->area);
	gtk_box_append(GTK_BOX(t->box), t->bar);

	t->im = gtk_im_multicontext_new();
	g_signal_connect(t->im, "commit", G_CALLBACK(committed), t);
	GtkEventController *k = gtk_event_controller_key_new();
	gtk_event_controller_key_set_im_context(GTK_EVENT_CONTROLLER_KEY(k), t->im);
	g_signal_connect(k, "key-pressed", G_CALLBACK(key_pressed), t);
	gtk_widget_add_controller(t->area, k);
	gtk_im_context_set_client_widget(t->im, t->area);

	GtkGesture *g = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g), 0);
	g_signal_connect(g, "pressed", G_CALLBACK(pressed), t);
	g_signal_connect(g, "released", G_CALLBACK(released), t);
	gtk_widget_add_controller(t->area, GTK_EVENT_CONTROLLER(g));
	GtkEventController *m = gtk_event_controller_motion_new();
	g_signal_connect(m, "motion", G_CALLBACK(moved), t);
	gtk_widget_add_controller(t->area, m);
	GtkEventController *s = gtk_event_controller_scroll_new(
		GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
	g_signal_connect(s, "scroll", G_CALLBACK(scrolled), t);
	gtk_widget_add_controller(t->area, s);
	GtkEventController *f = gtk_event_controller_focus_new();
	g_signal_connect(f, "enter", G_CALLBACK(focus_in), t);
	g_signal_connect(f, "leave", G_CALLBACK(focus_out), t);
	gtk_widget_add_controller(t->area, f);

	g_object_set_data_full(G_OBJECT(t->box), "termview", t, destroyed);
	measure(t);
	t->cw = t->fcw;
	t->ch = t->fch;
	t->blink_tick = g_timeout_add(500, blink, t);
	return t->box;
}

void termview_feed(GtkWidget *w, const char *data, int n)
{
	TermView *t = of(w);
	if (!t || n <= 0)
		return;
	int before = dmc_vt_history(t->vt);
	dmc_vt_feed(t->vt, data, n);
	if (t->scroll)
		t->scroll = MAX(t->scroll - (dmc_vt_history(t->vt) - before),
				-dmc_vt_history(t->vt));
	char r[256];
	int k;
	while ((k = dmc_vt_reply(t->vt, r, sizeof(r))) > 0)
		send_text(t, r, k);
	while (dmc_vt_printed(t->vt, r, sizeof(r)) > 0)
		;
	uint32_t hz;
	int ms, rest;
	while (dmc_vt_music(t->vt, &hz, &ms, &rest))
		;
	if (dmc_vt_bells(t->vt))
		gtk_widget_error_bell(t->area);
	layout(t);
	update_bar(t);
	if (dmc_vt_changed(t->vt))
		gtk_widget_queue_draw(t->area);
}

void termview_reset(GtkWidget *w)
{
	TermView *t = of(w);
	if (!t)
		return;
	dmc_vt_reset(t->vt);
	t->scroll = 0;
	layout(t);
	update_bar(t);
	gtk_widget_queue_draw(t->area);
}

void termview_size(GtkWidget *w, int *cols, int *rows)
{
	TermView *t = of(w);
	*cols = t ? dmc_vt_cols(t->vt) : 80;
	*rows = t ? dmc_vt_rows(t->vt) : 24;
}

void termview_set_backspace(GtkWidget *w, int code)
{
	TermView *t = of(w);
	if (t)
		dmc_vt_set_backspace(t->vt, code);
}

void termview_set_profile(GtkWidget *w, const char *id)
{
	TermView *t = of(w);
	if (!t || !dmc_vt_set_profile(t->vt, id))
		return;
	t->scroll = 0;
	layout(t);
	update_bar(t);
	gtk_widget_queue_draw(t->area);
}

const char *termview_profile(GtkWidget *w)
{
	TermView *t = of(w);
	return t ? dmc_vt_get_profile(t->vt) : "xterm";
}

void termview_focus(GtkWidget *w)
{
	TermView *t = of(w);
	if (t)
		gtk_widget_grab_focus(t->area);
}

GtkWidget *term_types_new(const char *id)
{
	int n = dmc_vt_profile_count(), sel = 0;
	const char **labels = g_new0(const char *, n + 1);
	for (int i = 0; i < n; i++) {
		labels[i] = dmc_vt_profile_label(i);
		if (id && !strcmp(id, dmc_vt_profile_id(i)))
			sel = i;
	}
	GtkWidget *d = gtk_drop_down_new_from_strings(labels);
	g_free(labels);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(d), (guint)sel);
	gtk_widget_set_tooltip_text(d, "The kind of terminal");
	return d;
}

const char *term_types_chosen(GtkWidget *d)
{
	const char *id = dmc_vt_profile_id(
		(int)gtk_drop_down_get_selected(GTK_DROP_DOWN(d)));
	return id ? id : dmc_vt_profile_id(0);
}
