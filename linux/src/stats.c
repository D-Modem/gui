/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * stats.c - the Call Stats page: the last call's statistics (AT#UD, as
 *           labelled rows) and the modem's own report of it (ATI11)
 */
#include "app.h"

static void got_report(App *a, const dmc_event *ev, gpointer data)
{
	(void)data;
	gtk_label_set_text(GTK_LABEL(a->stats_report),
			   ev->ok ? ev->text : "");
}

static void got_diag(App *a, const dmc_event *ev, gpointer data)
{
	(void)data;
	for (guint i = 0; i < a->stats_rows->len; i++)
		adw_preferences_group_remove(a->stats_group,
					     g_ptr_array_index(a->stats_rows, i));
	g_ptr_array_set_size(a->stats_rows, 0);
	if (!ev->ok) {
		gtk_widget_set_visible(a->stats_empty, TRUE);
		return;
	}
	dmc_diag *d = dmc_diag_parse(ev->text);
	gboolean call = dmc_diag_has_call(d);
	gtk_widget_set_visible(a->stats_empty, !call);
	for (int i = 0; call && i < dmc_diag_count(d); i++) {
		if (dmc_diag_key(d, i) == 0)
			continue;       /* the report's version */
		GtkWidget *r = adw_action_row_new();
		adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(r), FALSE);
		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r),
					      dmc_diag_label(d, i));
		GtkWidget *v = gtk_label_new(dmc_diag_value(d, i));
		gtk_label_set_selectable(GTK_LABEL(v), TRUE);
		gtk_widget_add_css_class(v, "dim-label");
		adw_action_row_add_suffix(ADW_ACTION_ROW(r), v);
		adw_preferences_group_add(a->stats_group, r);
		g_ptr_array_add(a->stats_rows, r);
	}
	dmc_diag_free(d);
}

void stats_refresh(App *a)
{
	int st = dmc_state(a->s);
	if (st == DMC_CLOSED || st == DMC_ONLINE)
		return;
	modem_command(a, "AT#UD", DMC_QUIET, got_diag, NULL);
	modem_command(a, "ATI11", DMC_QUIET, got_report, NULL);
}

static void refresh(GtkWidget *w, gpointer data)
{
	(void)w;
	stats_refresh(data);
}

GtkWidget *stats_page_new(App *a)
{
	a->stats_rows = g_ptr_array_new();
	GtkWidget *page = adw_preferences_page_new();
	GtkWidget *g = adw_preferences_group_new();
	a->stats_group = ADW_PREFERENCES_GROUP(g);
	adw_preferences_group_set_title(a->stats_group, "Last call");
	adw_preferences_group_set_description(a->stats_group,
		"As the modem reports it (AT#UD), after each call; during "
		"one, escape to command mode to see it live.");
	GtkWidget *b = gtk_button_new_from_icon_name("view-refresh-symbolic");
	gtk_widget_set_tooltip_text(b, "Read them again");
	gtk_widget_add_css_class(b, "flat");
	g_signal_connect(b, "clicked", G_CALLBACK(refresh), a);
	adw_preferences_group_set_header_suffix(a->stats_group, b);
	a->stats_empty = gtk_label_new("No call yet.");
	gtk_widget_add_css_class(a->stats_empty, "dim-label");
	adw_preferences_group_add(a->stats_group, a->stats_empty);
	adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), a->stats_group);

	GtkWidget *rg = adw_preferences_group_new();
	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(rg),
					"The modem's report (ATI11)");
	a->stats_report = gtk_label_new("");
	gtk_label_set_selectable(GTK_LABEL(a->stats_report), TRUE);
	gtk_label_set_xalign(GTK_LABEL(a->stats_report), 0);
	gtk_widget_add_css_class(a->stats_report, "monospace");
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(rg), a->stats_report);
	adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
				 ADW_PREFERENCES_GROUP(rg));
	return page;
}
