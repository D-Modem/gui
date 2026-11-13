/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * pathview.c - the connection path at the top of the Settings page, as
 *              PCOMM's "Connection Overview" draws its link: each step's
 *              role above its icon, what it is below, one line joining
 *              them (libdmodemctl's dmc_path).  Idle, how the line
 *              settings set it up; starting, calling and online, as it is.
 */
#include "app.h"

#define STEPS 4

typedef struct PathView {
	dmc_path *path;
	GtkWidget *role[STEPS], *line[STEPS], *icon[STEPS], *title[STEPS],
		*detail[STEPS], *headline;
	int state[STEPS], left[STEPS], right[STEPS];
} PathView;

static PathView pv;

static const char *icon_name(int kind)
{
	switch (kind) {
	case DMC_KIND_TERMINAL: return "utilities-terminal-symbolic";
	case DMC_KIND_SOFTMODEM:
	case DMC_KIND_MODEM: return "modem-symbolic";
	case DMC_KIND_SIP: return "call-start-symbolic";
	case DMC_KIND_USB: return "media-removable-symbolic";
	case DMC_KIND_CODEC: return "audio-card-symbolic";
	case DMC_KIND_SERIAL: return "network-wired-symbolic";
	case DMC_KIND_NETWORK: return "network-workgroup-symbolic";
	case DMC_KIND_SIP_ADDRESS: return "send-to-symbolic";
	default: return "phone-symbolic";
	}
}

/* a state's colour: ready the accent, to set red, working and active
   green (libadwaita's), off the text's, faint */
static void state_rgba(GtkWidget *w, int state, GdkRGBA *c)
{
	switch (state) {
	case DMC_STEP_SETUP: gdk_rgba_parse(c, "#e01b24"); break;
	case DMC_STEP_WORKING:
	case DMC_STEP_ACTIVE: gdk_rgba_parse(c, "#26a269"); break;
	case DMC_STEP_OFF:
		gtk_widget_get_color(w, c);
		c->alpha = 0.3f;
		break;
	default: gdk_rgba_parse(c, "#3584e4"); break;
	}
}

static const char *state_class(int state)
{
	switch (state) {
	case DMC_STEP_SETUP: return "error";
	case DMC_STEP_WORKING:
	case DMC_STEP_ACTIVE: return "success";
	case DMC_STEP_OFF: return "dim-label";
	default: return "accent";
	}
}

/* step i's icon row: the line through its middle, half to the step before
   and half to the next (dashed where one is still to set, or off), and
   the ring the icon sits in */
static void draw_step(GtkDrawingArea *da, cairo_t *cr, int w, int h,
		      gpointer data)
{
	int i = GPOINTER_TO_INT(data);
	double y = h / 2.0, r = 22;
	static const double dash[] = { 6, 4 };
	GdkRGBA c;
	cairo_set_line_width(cr, 2);
	for (int side = 0; side < 2; side++) {
		int st = side ? pv.right[i] : pv.left[i];
		if (st < 0)
			continue;
		state_rgba(GTK_WIDGET(da), st, &c);
		gdk_cairo_set_source_rgba(cr, &c);
		cairo_set_dash(cr, dash, st == DMC_STEP_SETUP ||
			       st == DMC_STEP_OFF ? 2 : 0, 0);
		cairo_move_to(cr, side ? w / 2.0 + r : 0, y);
		cairo_line_to(cr, side ? w : w / 2.0 - r, y);
		cairo_stroke(cr);
	}
	cairo_set_dash(cr, NULL, 0, 0);
	state_rgba(GTK_WIDGET(da), pv.state[i], &c);
	cairo_arc(cr, w / 2.0, y, r, 0, 2 * G_PI);
	if (pv.state[i] == DMC_STEP_ACTIVE) {
		GdkRGBA f = c;
		f.alpha = 0.2f;
		gdk_cairo_set_source_rgba(cr, &f);
		cairo_fill_preserve(cr);
	}
	gdk_cairo_set_source_rgba(cr, &c);
	cairo_stroke(cr);
}

static void set_class(GtkWidget *w, const char *cls)
{
	static const char *const all[] = { "error", "success", "dim-label",
					   "accent" };
	for (guint k = 0; k < G_N_ELEMENTS(all); k++)
		gtk_widget_remove_css_class(w, all[k]);
	gtk_widget_add_css_class(w, cls);
}

void pathview_update(App *a)
{
	if (!pv.path)
		return;
	const Config *c = &a->cfg;
	int line = DMC_LINE_SIP;
	const char *where = "";
	if (!g_strcmp0(c->line, "usb")) {
		line = DMC_LINE_USB;
		where = c->usb;
	} else if (!g_strcmp0(c->line, "alsa")) {
		line = DMC_LINE_CODEC;
		where = c->alsa;
	} else if (!g_strcmp0(c->line, "device")) {
		line = DMC_LINE_DEVICE;
		where = c->device;
	}
	dmc_path_set_line(pv.path, line, where, c->sip_anonymous, c->sip_user,
			  c->sip_server, c->listen);
	int st = a->s ? dmc_state(a->s) : DMC_CLOSED;
	dmc_path_set_live(pv.path, modem_running(a), st,
			  st == DMC_ONLINE || st == DMC_ONLINE_COMMAND ?
			  dmc_connect_text(a->s) : "");
	int n = dmc_path_steps(pv.path);
	GString *spoken = g_string_new(dmc_path_headline(pv.path));
	for (int i = 0; i < STEPS && i < n; i++) {
		pv.state[i] = dmc_path_state(pv.path, i);
		pv.left[i] = i > 0 ? dmc_path_link(pv.path, i - 1) : -1;
		pv.right[i] = i < n - 1 ? dmc_path_link(pv.path, i) : -1;
		gtk_label_set_text(GTK_LABEL(pv.role[i]), dmc_path_role(pv.path, i));
		gtk_label_set_text(GTK_LABEL(pv.title[i]), dmc_path_title(pv.path, i));
		gtk_label_set_text(GTK_LABEL(pv.detail[i]), dmc_path_detail(pv.path, i));
		gtk_image_set_from_icon_name(GTK_IMAGE(pv.icon[i]),
					     icon_name(dmc_path_kind(pv.path, i)));
		set_class(pv.icon[i], state_class(pv.state[i]));
		if (pv.state[i] == DMC_STEP_SETUP)
			gtk_widget_add_css_class(pv.detail[i], "error");
		else
			gtk_widget_remove_css_class(pv.detail[i], "error");
		gtk_widget_queue_draw(pv.line[i]);
		g_string_append_printf(spoken, ". %s: %s, %s",
				       dmc_path_role(pv.path, i),
				       dmc_path_title(pv.path, i),
				       dmc_path_detail(pv.path, i));
	}
	gtk_label_set_text(GTK_LABEL(pv.headline), dmc_path_headline(pv.path));
	set_class(pv.headline, dmc_path_needs_setup(pv.path) ? "error" :
		  "dim-label");
	gtk_accessible_update_property(GTK_ACCESSIBLE(gtk_widget_get_parent(pv.headline)),
				       GTK_ACCESSIBLE_PROPERTY_LABEL, spoken->str, -1);
	g_string_free(spoken, TRUE);
}

static GtkWidget *label(const char *cls, gboolean wrap)
{
	GtkWidget *l = gtk_label_new("");
	gtk_label_set_justify(GTK_LABEL(l), GTK_JUSTIFY_CENTER);
	if (wrap) {
		gtk_label_set_wrap(GTK_LABEL(l), TRUE);
		gtk_label_set_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
		gtk_label_set_max_width_chars(GTK_LABEL(l), 18);
		gtk_label_set_lines(GTK_LABEL(l), 3);
		gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
	}
	if (cls)
		gtk_widget_add_css_class(l, cls);
	return l;
}

GtkWidget *pathview_new(App *a)
{
	pv.path = dmc_path_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_add_css_class(box, "card");
	gtk_widget_set_margin_bottom(box, 6);
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_box_set_homogeneous(GTK_BOX(row), TRUE);
	gtk_widget_set_margin_top(row, 12);
	for (int i = 0; i < STEPS; i++) {
		GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
		pv.role[i] = label("caption", FALSE);
		gtk_widget_add_css_class(pv.role[i], "dim-label");
		gtk_box_append(GTK_BOX(col), pv.role[i]);
		GtkWidget *ov = gtk_overlay_new();
		pv.line[i] = gtk_drawing_area_new();
		gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(pv.line[i]), 48);
		gtk_widget_set_hexpand(pv.line[i], TRUE);
		gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(pv.line[i]),
					       draw_step, GINT_TO_POINTER(i), NULL);
		gtk_overlay_set_child(GTK_OVERLAY(ov), pv.line[i]);
		pv.icon[i] = gtk_image_new();
		gtk_image_set_pixel_size(GTK_IMAGE(pv.icon[i]), 20);
		gtk_widget_set_halign(pv.icon[i], GTK_ALIGN_CENTER);
		gtk_widget_set_valign(pv.icon[i], GTK_ALIGN_CENTER);
		gtk_overlay_add_overlay(GTK_OVERLAY(ov), pv.icon[i]);
		gtk_box_append(GTK_BOX(col), ov);
		pv.title[i] = label("heading", FALSE);
		gtk_box_append(GTK_BOX(col), pv.title[i]);
		pv.detail[i] = label("caption", TRUE);
		gtk_widget_set_margin_start(pv.detail[i], 4);
		gtk_widget_set_margin_end(pv.detail[i], 4);
		gtk_box_append(GTK_BOX(col), pv.detail[i]);
		gtk_box_append(GTK_BOX(row), col);
	}
	gtk_box_append(GTK_BOX(box), row);
	pv.headline = label(NULL, TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(pv.headline), 60);
	gtk_widget_set_margin_bottom(pv.headline, 12);
	gtk_widget_set_margin_start(pv.headline, 12);
	gtk_widget_set_margin_end(pv.headline, 12);
	gtk_box_append(GTK_BOX(box), pv.headline);
	pathview_update(a);
	return box;
}
