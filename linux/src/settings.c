/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * settings.c - the Settings page: the modem's own settings (AT!M), one
 *              row each, grouped as the configurators group them
 *
 * A change runs the setting's command at once, as the configurators do;
 * a value the modem refuses is put back.  AT!R's init string, the
 * commands that set what differs from the defaults, can be kept as the
 * commands the app sends when the modem opens.
 */
#include <stdlib.h>
#include <string.h>

#include "app.h"

typedef struct Row {
	App *a;
	int i;                  /* the setting's index in the model */
	GtkWidget *w;
} Row;

static void set_row(App *a, Row *r, const char *value);
static void mark(App *a, Row *r);

/* a change sent to the modem */
typedef struct Change {
	int gen, i;
	char *value;
} Change;

static void applied(App *a, const dmc_event *ev, gpointer data)
{
	Change *c = data;
	if (a->model && c->gen == a->settings_gen) {
		Row *r = g_ptr_array_index(a->setting_rows, (guint)c->i);
		if (ev->ok) {
			dmc_setting_set(a->model, c->i, c->value);
			mark(a, r);
		} else {
			char *m = g_strdup_printf("The modem refused %s",
						  dmc_setting_label(a->model, c->i));
			window_toast(a, m);
			g_free(m);
			set_row(a, r, dmc_setting_value(a->model, c->i));
		}
		/* the country sets other settings' defaults too */
		if (ev->ok && !strcmp(dmc_setting_key(a->model, c->i),
				      "country"))
			settings_load(a);
	}
	g_free(c->value);
	g_free(c);
}

static void apply(Row *r, const char *value)
{
	App *a = r->a;
	if (a->settings_quiet || !a->model)
		return;
	if (!strcmp(value, dmc_setting_value(a->model, r->i)))
		return;
	const char *why = dmc_setting_check(a->model, r->i, value);
	if (*why) {
		window_toast(a, why);
		set_row(a, r, dmc_setting_value(a->model, r->i));
		return;
	}
	Change *c = g_new0(Change, 1);
	c->gen = a->settings_gen;
	c->i = r->i;
	c->value = g_strdup(value);
	const char *cmd = dmc_setting_command(a->model, r->i, value);
	if (modem_command(a, cmd, 0, applied, c) < 0) {
		window_toast(a, "The modem is busy");
		g_free(c->value);
		g_free(c);
		set_row(a, r, dmc_setting_value(a->model, r->i));
	}
}

static void picked(GObject *o, GParamSpec *ps, gpointer data)
{
	(void)ps;
	Row *r = data;
	guint sel = adw_combo_row_get_selected(ADW_COMBO_ROW(o));
	if (sel == GTK_INVALID_LIST_POSITION || !r->a->model)
		return;
	apply(r, dmc_choice_value(r->a->model, r->i, (int)sel));
}

static void spun(GObject *o, GParamSpec *ps, gpointer data)
{
	(void)ps;
	Row *r = data;
	char v[32];
	g_snprintf(v, sizeof(v), "%.0f", adw_spin_row_get_value(ADW_SPIN_ROW(o)));
	apply(r, v);
}

static void entered(AdwEntryRow *e, gpointer data)
{
	apply(data, gtk_editable_get_text(GTK_EDITABLE(e)));
}

static void set_row(App *a, Row *r, const char *value)
{
	a->settings_quiet = TRUE;
	switch (dmc_setting_kind(a->model, r->i)) {
	case DMC_PICK: {
		int c = dmc_setting_choice_of(a->model, r->i, value);
		adw_combo_row_set_selected(ADW_COMBO_ROW(r->w),
					   c < 0 ? GTK_INVALID_LIST_POSITION :
					   (guint)c);
		break;
	}
	case DMC_NUMBER:
		adw_spin_row_set_value(ADW_SPIN_ROW(r->w), atof(value));
		break;
	default:
		gtk_editable_set_text(GTK_EDITABLE(r->w), value);
		break;
	}
	a->settings_quiet = FALSE;
}

/* a row shows when its value is not the default */
static void mark(App *a, Row *r)
{
	gboolean changed = strcmp(dmc_setting_value(a->model, r->i),
				  dmc_setting_default(a->model, r->i)) != 0;
	if (changed)
		gtk_widget_add_css_class(r->w, "accent");
	else
		gtk_widget_remove_css_class(r->w, "accent");
}

static GtkWidget *row_for(App *a, int i)
{
	dmc_model *m = a->model;
	Row *r = g_new0(Row, 1);
	r->a = a;
	r->i = i;
	switch (dmc_setting_kind(m, i)) {
	case DMC_PICK: {
		GtkStringList *l = gtk_string_list_new(NULL);
		for (int c = 0; c < dmc_setting_choices(m, i); c++)
			gtk_string_list_append(l, dmc_choice_label(m, i, c));
		r->w = adw_combo_row_new();
		adw_combo_row_set_model(ADW_COMBO_ROW(r->w), G_LIST_MODEL(l));
		g_object_unref(l);
		if (dmc_setting_choices(m, i) > 12)
			adw_combo_row_set_enable_search(ADW_COMBO_ROW(r->w), TRUE);
		set_row(a, r, dmc_setting_value(m, i));
		g_signal_connect(r->w, "notify::selected", G_CALLBACK(picked), r);
		break;
	}
	case DMC_NUMBER:
		r->w = adw_spin_row_new_with_range((double)dmc_setting_lo(m, i),
						   (double)dmc_setting_hi(m, i), 1);
		set_row(a, r, dmc_setting_value(m, i));
		g_signal_connect(r->w, "notify::value", G_CALLBACK(spun), r);
		break;
	default:
		r->w = adw_entry_row_new();
		adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(r->w), TRUE);
		set_row(a, r, dmc_setting_value(m, i));
		g_signal_connect(r->w, "apply", G_CALLBACK(entered), r);
		break;
	}
	adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(r->w), FALSE);
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r->w),
				      dmc_setting_label(m, i));
	if (ADW_IS_ACTION_ROW(r->w))
		adw_action_row_set_subtitle(ADW_ACTION_ROW(r->w),
					    dmc_setting_help(m, i));
	else
		gtk_widget_set_tooltip_text(r->w, dmc_setting_help(m, i));
	g_object_set_data_full(G_OBJECT(r->w), "row", r, g_free);
	g_ptr_array_add(a->setting_rows, r);
	mark(a, r);
	return r->w;
}

void settings_clear(App *a)
{
	for (guint g = 0; a->settings_groups && g < a->settings_groups->len; g++)
		adw_preferences_page_remove(a->settings_page,
					    g_ptr_array_index(a->settings_groups, g));
	if (a->settings_groups)
		g_ptr_array_set_size(a->settings_groups, 0);
	if (a->setting_rows)
		g_ptr_array_set_size(a->setting_rows, 0);
	a->settings_gen++;
	dmc_model_free(a->model);
	a->model = NULL;
	gtk_label_set_text(GTK_LABEL(a->settings_status),
			   "Start the modem to see its settings.");
	gtk_widget_set_visible(a->settings_status, TRUE);
}

static void got_init(App *a, const dmc_event *ev, gpointer data)
{
	(void)data;
	if (!ev->ok)
		return;
	/* AT!R's lines, as one line of commands */
	GString *s = g_string_new(NULL);
	char **lines = g_strsplit(ev->text, "\n", -1);
	for (int i = 0; lines[i]; i++) {
		char *l = g_strstrip(lines[i]);
		if (!*l || !g_ascii_strcasecmp(l, "AT"))
			continue;
		if (s->len) {
			/* (the next line's commands after this one's) */
			g_string_append(s, l + 2);
		} else {
			g_string_append(s, l);
		}
	}
	g_strfreev(lines);
	adw_action_row_set_subtitle(ADW_ACTION_ROW(a->init_row),
				    s->len ? s->str : "AT (all at their defaults)");
	g_object_set_data_full(G_OBJECT(a->init_row), "init",
			       g_string_free(s, FALSE), g_free);
}

static void got_model(App *a, const dmc_event *ev, gpointer data)
{
	(void)data;
	if (!ev->ok) {
		gtk_label_set_text(GTK_LABEL(a->settings_status),
				   "This modem does not list its settings "
				   "(AT!M): update D-Modem, or use the "
				   "Terminal's setup screen.");
		return;
	}
	dmc_model *m = dmc_model_parse(ev->text);
	if (!m) {
		gtk_label_set_text(GTK_LABEL(a->settings_status),
				   "The modem's settings list was not readable.");
		return;
	}
	settings_clear(a);
	a->model = m;
	gtk_widget_set_visible(a->settings_status, FALSE);
	for (int g = 0; g < dmc_model_groups(m); g++) {
		const char *name = dmc_model_group(m, g);
		GtkWidget *grp = adw_preferences_group_new();
		adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(grp), name);
		for (int i = 0; i < dmc_model_count(m); i++)
			if (!strcmp(dmc_setting_group(m, i), name))
				adw_preferences_group_add(ADW_PREFERENCES_GROUP(grp),
							  row_for(a, i));
		adw_preferences_page_add(a->settings_page,
					 ADW_PREFERENCES_GROUP(grp));
		g_ptr_array_add(a->settings_groups, grp);
	}
	modem_command(a, "AT!R", DMC_QUIET, got_init, NULL);
	settings_sensitive(a, dmc_state(a->s) == DMC_IDLE);
}

void settings_load(App *a)
{
	if (dmc_state(a->s) == DMC_CLOSED)
		return;
	modem_command(a, "AT!M", DMC_QUIET, got_model, NULL);
}

void settings_sensitive(App *a, gboolean on)
{
	for (guint g = 0; a->settings_groups && g < a->settings_groups->len; g++)
		gtk_widget_set_sensitive(g_ptr_array_index(a->settings_groups, g),
					 on);
}

static void reload(GtkWidget *w, gpointer data)
{
	(void)w;
	settings_load(data);
}

static void reset_done(App *a, const dmc_event *ev, gpointer data)
{
	(void)data;
	if (!ev->ok)
		window_toast(a, "The modem refused ATZ");
	settings_load(a);
}

static void reset(GtkWidget *w, gpointer data)
{
	(void)w;
	App *a = data;
	modem_command(a, "ATZ", 0, reset_done, NULL);
}

static void keep_init(GtkWidget *w, gpointer data)
{
	(void)w;
	App *a = data;
	const char *init = g_object_get_data(G_OBJECT(a->init_row), "init");
	g_free(a->cfg.init);
	a->cfg.init = g_strdup(init ? init : "");
	config_save(&a->cfg);
	window_toast(a, *a->cfg.init ? "Sent each time the modem opens" :
		     "Nothing to send: every setting is at its default");
}

GtkWidget *settings_page_new(App *a)
{
	a->settings_groups = g_ptr_array_new();
	a->setting_rows = g_ptr_array_new();
	a->settings_page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());

	GtkWidget *top = adw_preferences_group_new();
	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(top), "The modem's settings");
	adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(top),
		"Changes take effect at once. The modem keeps no stored "
		"profile, so keep the init string to have them each time.");
	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *b = gtk_button_new_from_icon_name("view-refresh-symbolic");
	gtk_widget_set_tooltip_text(b, "Read the settings again");
	gtk_widget_add_css_class(b, "flat");
	g_signal_connect(b, "clicked", G_CALLBACK(reload), a);
	gtk_box_append(GTK_BOX(bar), b);
	b = gtk_button_new_with_label("Defaults");
	gtk_widget_set_tooltip_text(b, "Every setting back to its default (ATZ)");
	gtk_widget_add_css_class(b, "flat");
	g_signal_connect(b, "clicked", G_CALLBACK(reset), a);
	gtk_box_append(GTK_BOX(bar), b);
	adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(top), bar);

	a->init_row = adw_action_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(a->init_row),
				      "Init string");
	adw_action_row_set_subtitle(ADW_ACTION_ROW(a->init_row), "");
	adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(a->init_row), TRUE);
	gtk_widget_add_css_class(a->init_row, "property");
	b = gtk_button_new_with_label("Keep");
	gtk_widget_set_tooltip_text(b, "Send it each time the modem opens");
	gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
	g_signal_connect(b, "clicked", G_CALLBACK(keep_init), a);
	adw_action_row_add_suffix(ADW_ACTION_ROW(a->init_row), b);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(top), a->init_row);
	a->settings_status = gtk_label_new("");
	gtk_label_set_wrap(GTK_LABEL(a->settings_status), TRUE);
	gtk_widget_add_css_class(a->settings_status, "dim-label");
	gtk_widget_set_margin_top(a->settings_status, 12);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(top), a->settings_status);
	adw_preferences_page_add(a->settings_page, ADW_PREFERENCES_GROUP(top));
	settings_clear(a);
	return GTK_WIDGET(a->settings_page);
}
