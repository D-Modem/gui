/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * window.c - the main window: the Modem, Phone, Terminal, Remote,
 *            Settings, Call and Log pages, the same pages as the Windows and macOS apps
 *            (docs/design.md)
 */
#include <string.h>

#include "app.h"

void window_toast(App *a, const char *text)
{
	AdwToast *t = adw_toast_new(text);
	adw_toast_set_timeout(t, 4);
	adw_toast_overlay_add_toast(a->toasts, t);
}

static const char *state_icon(int state)
{
	switch (state) {
	case DMC_RINGING:
		return "call-incoming-symbolic";
	case DMC_DIALING:
	case DMC_ANSWERING:
		return "call-outgoing-symbolic";
	case DMC_ONLINE:
	case DMC_ONLINE_COMMAND:
		return "network-transmit-receive-symbolic";
	case DMC_CONFIGURATOR:
		return "preferences-system-symbolic";
	case DMC_VOICE:
		return "audio-input-microphone-symbolic";
	case DMC_VOICEMAIL:
		return "media-record-symbolic";
	default:
		return "call-start-symbolic";
	}
}

/* the Modem page and the buttons as the state is now */
void window_state(App *a)
{
	int st = dmc_state(a->s);
	gboolean running = modem_running(a);
	gtk_image_set_from_icon_name(GTK_IMAGE(a->status_icon),
				     running ? state_icon(st) :
				     "network-offline-symbolic");
	const char *title = running && st == DMC_CLOSED ? "Starting" :
		dmc_state_name(st);
	gtk_label_set_text(GTK_LABEL(a->status_title), title);
	char *detail = NULL;
	if (st == DMC_ONLINE || st == DMC_ONLINE_COMMAND ||
	    st == DMC_HANGING_UP)
		detail = g_strdup(dmc_connect_text(a->s));
	else if (st == DMC_RINGING && *dmc_caller_number(a->s))
		detail = g_strdup_printf("%s %s", dmc_caller_number(a->s),
					 dmc_caller_name(a->s));
	else
		detail = config_summary(&a->cfg);
	gtk_label_set_text(GTK_LABEL(a->status_detail), detail);
	g_free(detail);

	gboolean open = st != DMC_CLOSED;
	gtk_widget_set_sensitive(a->dial_btn, open && st == DMC_IDLE);
	gtk_widget_set_sensitive(a->number, open);
	gtk_widget_set_sensitive(a->answer_btn, st == DMC_RINGING);
	gtk_widget_set_sensitive(a->hangup_btn,
				 st == DMC_DIALING || st == DMC_ANSWERING ||
				 st == DMC_ONLINE || st == DMC_ONLINE_COMMAND ||
				 st == DMC_RINGING);
	gtk_button_set_label(GTK_BUTTON(a->run_btn), running ? "Stop" : "Start");
	if (running)
		gtk_widget_remove_css_class(a->run_btn, "suggested-action");
	else
		gtk_widget_add_css_class(a->run_btn, "suggested-action");
	char *sum = config_summary(&a->cfg);
	adw_action_row_set_subtitle(ADW_ACTION_ROW(a->run_row), sum);
	g_free(sum);
	adw_action_row_set_subtitle(ADW_ACTION_ROW(a->device_row),
				    a->fd >= 0 && a->device ? a->device :
				    "Not running");
	gtk_widget_set_sensitive(a->copy_btn, a->fd >= 0);
	gtk_widget_set_sensitive(a->term_buttons, st == DMC_IDLE);
	settings_sensitive(a, st == DMC_IDLE);
	voice_state(a);
}

void window_opened(App *a)
{
	window_state(a);
	settings_load(a);
	stats_refresh(a);
}

void window_closed(App *a)
{
	settings_clear(a);
	window_state(a);
}

void window_event(App *a, const dmc_event *ev)
{
	switch (ev->type) {
	case DMC_EV_STATE: {
		int was = a->last_state;
		a->last_state = ev->state;
		window_state(a);
		/* a call over: its statistics */
		if (ev->state == DMC_IDLE &&
		    (was == DMC_ONLINE || was == DMC_ONLINE_COMMAND ||
		     was == DMC_HANGING_UP || was == DMC_DIALING ||
		     was == DMC_ANSWERING))
			stats_refresh(a);
		/* a configurator over: the settings it changed */
		if (ev->state == DMC_IDLE && was == DMC_CONFIGURATOR)
			settings_load(a);
		break;
	}
	case DMC_EV_RESULT:
		if (!strcmp(ev->result, "RING")) {
			if (!gtk_window_is_active(a->window)) {
				GNotification *n = g_notification_new("Incoming call");
				const char *who = dmc_caller_number(a->s);
				g_notification_set_body(n, *who ? who :
							"The modem is ringing");
				g_application_send_notification(
					G_APPLICATION(a->gapp), "ring", n);
				g_object_unref(n);
			}
		} else if (ev->result[0]) {
			window_toast(a, ev->result);
		}
		break;
	case DMC_EV_CALLER:
		window_state(a);
		break;
	case DMC_EV_VOICE:
	case DMC_EV_MESSAGE:
		voice_event(a, ev);
		break;
	}
}

/*
 *    the Modem page
 */

static void dialled(App *a, const dmc_event *ev, gpointer data)
{
	(void)data;
	if (!ev->ok)
		window_toast(a, ev->result);
}

static void dial(GtkWidget *w, gpointer data)
{
	App *a = data;
	(void)w;
	const char *n = gtk_editable_get_text(GTK_EDITABLE(a->number));
	if (!*n) {
		window_toast(a, "Enter a number to dial");
		return;
	}
	char *cmd = g_strdup_printf("ATD%s", n);
	modem_command(a, cmd, 0, dialled, NULL);
	g_free(cmd);
}

static void answer(GtkWidget *w, gpointer data)
{
	(void)w;
	App *a = data;
	modem_command(a, "ATA", 0, dialled, NULL);
}

static void hangup(GtkWidget *w, gpointer data)
{
	(void)w;
	App *a = data;
	dmc_hangup(a->s);
	modem_pump(a);
}

static void run(GtkWidget *w, gpointer data)
{
	(void)w;
	App *a = data;
	if (modem_running(a))
		modem_stop(a);
	else
		modem_start(a);
}

static void copy_device(GtkWidget *w, gpointer data)
{
	App *a = data;
	if (!a->device)
		return;
	gdk_clipboard_set_text(gtk_widget_get_clipboard(w), a->device);
	window_toast(a, "Copied");
}

static void line_settings(GtkWidget *w, gpointer data)
{
	(void)w;
	prefs_show(data);
}

static GtkWidget *modem_page(App *a)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);
	gtk_widget_set_margin_top(box, 24);
	gtk_widget_set_margin_bottom(box, 24);
	gtk_widget_set_margin_start(box, 12);
	gtk_widget_set_margin_end(box, 12);

	/* the state, large */
	a->status_icon = gtk_image_new_from_icon_name("network-offline-symbolic");
	gtk_image_set_pixel_size(GTK_IMAGE(a->status_icon), 96);
	gtk_widget_add_css_class(a->status_icon, "dim-label");
	gtk_box_append(GTK_BOX(box), a->status_icon);
	a->status_title = gtk_label_new("Not connected");
	gtk_widget_add_css_class(a->status_title, "title-1");
	gtk_box_append(GTK_BOX(box), a->status_title);
	a->status_detail = gtk_label_new("");
	gtk_label_set_wrap(GTK_LABEL(a->status_detail), TRUE);
	gtk_widget_add_css_class(a->status_detail, "dim-label");
	gtk_box_append(GTK_BOX(box), a->status_detail);

	/* the call */
	GtkWidget *call = adw_preferences_group_new();
	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(call), "Call");
	a->number = adw_entry_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(a->number),
				      "Number to dial");
	gtk_editable_set_text(GTK_EDITABLE(a->number), "");
	a->dial_btn = gtk_button_new_with_label("Dial");
	gtk_widget_set_valign(a->dial_btn, GTK_ALIGN_CENTER);
	gtk_widget_add_css_class(a->dial_btn, "suggested-action");
	adw_entry_row_add_suffix(ADW_ENTRY_ROW(a->number), a->dial_btn);
	g_signal_connect(a->dial_btn, "clicked", G_CALLBACK(dial), a);
	g_signal_connect(a->number, "entry-activated", G_CALLBACK(dial), a);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(call), a->number);

	GtkWidget *calls = adw_action_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(calls), "Answer or hang up");
	a->answer_btn = gtk_button_new_with_label("Answer");
	gtk_widget_set_valign(a->answer_btn, GTK_ALIGN_CENTER);
	g_signal_connect(a->answer_btn, "clicked", G_CALLBACK(answer), a);
	a->hangup_btn = gtk_button_new_with_label("Hang Up");
	gtk_widget_set_valign(a->hangup_btn, GTK_ALIGN_CENTER);
	gtk_widget_add_css_class(a->hangup_btn, "destructive-action");
	g_signal_connect(a->hangup_btn, "clicked", G_CALLBACK(hangup), a);
	adw_action_row_add_suffix(ADW_ACTION_ROW(calls), a->answer_btn);
	adw_action_row_add_suffix(ADW_ACTION_ROW(calls), a->hangup_btn);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(call), calls);
	gtk_box_append(GTK_BOX(box), call);

	/* the modem */
	GtkWidget *mg = adw_preferences_group_new();
	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(mg), "Modem");
	GtkWidget *line_btn = gtk_button_new_from_icon_name("emblem-system-symbolic");
	gtk_widget_set_tooltip_text(line_btn, "Line settings");
	gtk_widget_add_css_class(line_btn, "flat");
	g_signal_connect(line_btn, "clicked", G_CALLBACK(line_settings), a);
	adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(mg),
						line_btn);
	a->run_row = adw_action_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(a->run_row), "dmodemd");
	a->run_btn = gtk_button_new_with_label("Start");
	gtk_widget_set_valign(a->run_btn, GTK_ALIGN_CENTER);
	g_signal_connect(a->run_btn, "clicked", G_CALLBACK(run), a);
	adw_action_row_add_suffix(ADW_ACTION_ROW(a->run_row), a->run_btn);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(mg), a->run_row);
	a->device_row = adw_action_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(a->device_row),
				      "Device");
	a->copy_btn = gtk_button_new_from_icon_name("edit-copy-symbolic");
	gtk_widget_set_tooltip_text(a->copy_btn, "Copy the device's path");
	gtk_widget_set_valign(a->copy_btn, GTK_ALIGN_CENTER);
	gtk_widget_add_css_class(a->copy_btn, "flat");
	g_signal_connect(a->copy_btn, "clicked", G_CALLBACK(copy_device), a);
	adw_action_row_add_suffix(ADW_ACTION_ROW(a->device_row), a->copy_btn);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(mg), a->device_row);
	gtk_box_append(GTK_BOX(box), mg);

	GtkWidget *clamp = adw_clamp_new();
	adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 560);
	adw_clamp_set_child(ADW_CLAMP(clamp), box);
	GtkWidget *scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
				       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);
	return scroll;
}

/*
 *    the Terminal page
 */

static void committed(VteTerminal *t, char *text, guint size, gpointer data)
{
	(void)t;
	modem_type(data, text, size);
}

static void typed_command(App *a, const char *cmd)
{
	if (dmc_state(a->s) != DMC_IDLE)
		return;
	modem_type(a, cmd, strlen(cmd));
	gtk_widget_grab_focus(GTK_WIDGET(a->term));
}

static void setup_screen(GtkWidget *w, gpointer data)
{
	(void)w;
	typed_command(data, "AT!CF\r");
}

static void setup_cli(GtkWidget *w, gpointer data)
{
	(void)w;
	typed_command(data, "AT!CT\r");
}

static void clear_term(GtkWidget *w, gpointer data)
{
	(void)w;
	App *a = data;
	vte_terminal_reset(a->term, TRUE, TRUE);
}

static GtkWidget *terminal_page(App *a)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_margin_start(bar, 6);
	gtk_widget_set_margin_end(bar, 6);
	gtk_widget_set_margin_top(bar, 6);
	gtk_widget_set_margin_bottom(bar, 6);
	a->term_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *b = gtk_button_new_with_label("Full-Screen Setup");
	gtk_widget_set_tooltip_text(b, "The modem's own setup screen (AT!CF)");
	g_signal_connect(b, "clicked", G_CALLBACK(setup_screen), a);
	gtk_box_append(GTK_BOX(a->term_buttons), b);
	b = gtk_button_new_with_label("Command-Line Setup");
	gtk_widget_set_tooltip_text(b, "The modem's IOS-style command line "
				    "(AT!CT)");
	g_signal_connect(b, "clicked", G_CALLBACK(setup_cli), a);
	gtk_box_append(GTK_BOX(a->term_buttons), b);
	gtk_box_append(GTK_BOX(bar), a->term_buttons);
	GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_hexpand(spacer, TRUE);
	gtk_box_append(GTK_BOX(bar), spacer);
	b = gtk_button_new_from_icon_name("edit-clear-all-symbolic");
	gtk_widget_set_tooltip_text(b, "Clear");
	gtk_widget_add_css_class(b, "flat");
	g_signal_connect(b, "clicked", G_CALLBACK(clear_term), a);
	gtk_box_append(GTK_BOX(bar), b);
	gtk_box_append(GTK_BOX(box), bar);

	a->term = VTE_TERMINAL(vte_terminal_new());
	vte_terminal_set_scrollback_lines(a->term, 5000);
	vte_terminal_set_size(a->term, 80, 25);
	/* (the modem takes S5, a backspace, not DEL) */
	vte_terminal_set_backspace_binding(a->term, VTE_ERASE_ASCII_BACKSPACE);
	PangoFontDescription *f = pango_font_description_from_string("Monospace 11");
	vte_terminal_set_font(a->term, f);
	pango_font_description_free(f);
	/* the brand's colours */
	GdkRGBA fg, bg;
	gdk_rgba_parse(&fg, "#e8f0ff");
	gdk_rgba_parse(&bg, "#0a0f1c");
	vte_terminal_set_colors(a->term, &fg, &bg, NULL, 0);
	gtk_widget_set_vexpand(GTK_WIDGET(a->term), TRUE);
	gtk_widget_set_hexpand(GTK_WIDGET(a->term), TRUE);
	g_signal_connect(a->term, "commit", G_CALLBACK(committed), a);
	GtkWidget *scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
				      GTK_WIDGET(a->term));
	gtk_widget_set_vexpand(scroll, TRUE);
	gtk_box_append(GTK_BOX(box), scroll);
	return box;
}

/*
 *    the Log page
 */

static GtkWidget *log_page(App *a)
{
	GtkWidget *view = gtk_text_view_new();
	gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
	gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 12);
	gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view), 12);
	a->log = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
	GtkWidget *scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
	return scroll;
}

/*
 *    the window
 */

static void about(GSimpleAction *act, GVariant *v, gpointer data)
{
	(void)act;
	(void)v;
	App *a = data;
	AdwDialog *d = adw_about_dialog_new();
	adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(d), "D-Modem");
	adw_about_dialog_set_application_icon(ADW_ABOUT_DIALOG(d), APP_ID);
	adw_about_dialog_set_developer_name(ADW_ABOUT_DIALOG(d),
					    "The D-Modem Authors");
	adw_about_dialog_set_version(ADW_ABOUT_DIALOG(d), "1.0.0");
	adw_about_dialog_set_comments(ADW_ABOUT_DIALOG(d),
		"The front panel of D-Modem, the open V.92 softmodem.");
	adw_about_dialog_set_website(ADW_ABOUT_DIALOG(d),
				     "https://github.com/D-Modem/gui");
	adw_about_dialog_set_license_type(ADW_ABOUT_DIALOG(d),
					  GTK_LICENSE_AGPL_3_0_ONLY);
	adw_dialog_present(d, GTK_WIDGET(a->window));
}

static void line_action(GSimpleAction *act, GVariant *v, gpointer data)
{
	(void)act;
	(void)v;
	prefs_show(data);
}

void window_build(App *a)
{
	AdwApplicationWindow *w = ADW_APPLICATION_WINDOW(
		adw_application_window_new(GTK_APPLICATION(a->gapp)));
	a->window = GTK_WINDOW(w);
	gtk_window_set_title(a->window, "D-Modem");
	gtk_window_set_default_size(a->window, 860, 680);

	const GActionEntry actions[] = {
		{ "about", about, NULL, NULL, NULL, { 0 } },
		{ "line", line_action, NULL, NULL, NULL, { 0 } },
	};
	g_action_map_add_action_entries(G_ACTION_MAP(w), actions,
					G_N_ELEMENTS(actions), a);

	a->stack = ADW_VIEW_STACK(adw_view_stack_new());
	adw_view_stack_add_titled_with_icon(a->stack, modem_page(a), "modem",
					    "Modem", "call-start-symbolic");
	adw_view_stack_add_titled_with_icon(a->stack, voice_page_new(a),
					    "phone", "Phone",
					    "audio-input-microphone-symbolic");
	adw_view_stack_add_titled_with_icon(a->stack, terminal_page(a),
					    "terminal", "Terminal",
					    "utilities-terminal-symbolic");
	adw_view_stack_add_titled_with_icon(a->stack, remote_page_new(a),
					    "remote", "Remote",
					    "network-server-symbolic");
	adw_view_stack_add_titled_with_icon(a->stack, settings_page_new(a),
					    "settings", "Settings",
					    "preferences-system-symbolic");
	adw_view_stack_add_titled_with_icon(a->stack, stats_page_new(a),
					    "call", "Call Stats",
					    "utilities-system-monitor-symbolic");
	adw_view_stack_add_titled_with_icon(a->stack, log_page(a), "log",
					    "Log", "text-x-generic-symbolic");

	GtkWidget *header = adw_header_bar_new();
	GtkWidget *sw = adw_view_switcher_new();
	adw_view_switcher_set_stack(ADW_VIEW_SWITCHER(sw), a->stack);
	adw_view_switcher_set_policy(ADW_VIEW_SWITCHER(sw),
				     ADW_VIEW_SWITCHER_POLICY_WIDE);
	adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), sw);
	GMenu *menu = g_menu_new();
	g_menu_append(menu, "Line Settings", "win.line");
	g_menu_append(menu, "About D-Modem", "win.about");
	GtkWidget *mb = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(mb), "open-menu-symbolic");
	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(mb), G_MENU_MODEL(menu));
	g_object_unref(menu);
	adw_header_bar_pack_end(ADW_HEADER_BAR(header), mb);

	GtkWidget *tv = adw_toolbar_view_new();
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), header);
	a->toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
	adw_toast_overlay_set_child(a->toasts, GTK_WIDGET(a->stack));
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), GTK_WIDGET(a->toasts));
	adw_application_window_set_content(w, tv);
	/* (a page to open on: screenshots and the docs) */
	const char *page = g_getenv("DMODEM_GUI_PAGE");
	if (page)
		adw_view_stack_set_visible_child_name(a->stack, page);
	window_state(a);
}
