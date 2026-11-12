/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * main.c - D-Modem for Linux and the BSDs: GTK 4 and libadwaita over
 *          libdmodemctl
 */
#include "app.h"

void modem_init(App *a);

static void activate(GApplication *g, gpointer data)
{
	App *a = data;
	if (a->window) {
		gtk_window_present(a->window);
		return;
	}
	window_build(a);
	gtk_window_present(a->window);
	/* a modem that runs already, or one this app starts */
	if (!g_strcmp0(a->cfg.line, "device") ||
	    (!g_strcmp0(a->cfg.line, "sip") && *a->cfg.sip_user))
		modem_start(a);
	(void)g;
}

static void shutdown(GApplication *g, gpointer data)
{
	App *a = data;
	(void)g;
	remote_stop(a);
	if (modem_running(a))
		modem_stop(a);
	g_cancellable_cancel(a->cancel);
}

int main(int argc, char **argv)
{
	App a = { 0 };
	config_load(&a.cfg);
	modem_init(&a);
	a.gapp = adw_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
	g_signal_connect(a.gapp, "activate", G_CALLBACK(activate), &a);
	g_signal_connect(a.gapp, "shutdown", G_CALLBACK(shutdown), &a);
	int r = g_application_run(G_APPLICATION(a.gapp), argc, argv);
	g_object_unref(a.gapp);
	return r;
}
