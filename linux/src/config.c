/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * config.c - the line settings: kept, and the dialog that changes them
 *
 * They are a key file only the user may read (the SIP password is in
 * it), ~/.config/dmodem-gui/settings.ini.
 */
#include <glib/gstdio.h>

#include "app.h"

static char *path(void)
{
	return g_build_filename(g_get_user_config_dir(), "dmodem-gui",
				"settings.ini", NULL);
}

static char *get_s(GKeyFile *k, const char *key, const char *def)
{
	char *v = g_key_file_get_string(k, "line", key, NULL);
	return v ? v : g_strdup(def);
}

static gboolean get_b(GKeyFile *k, const char *key, gboolean def)
{
	GError *e = NULL;
	gboolean v = g_key_file_get_boolean(k, "line", key, &e);
	if (e) {
		g_error_free(e);
		return def;
	}
	return v;
}

void config_load(Config *c)
{
	GKeyFile *k = g_key_file_new();
	char *p = path();
	g_key_file_load_from_file(k, p, G_KEY_FILE_NONE, NULL);
	g_free(p);
	c->line = get_s(k, "line", "sip");
	c->sip_user = get_s(k, "sip-user", "");
	c->sip_password = get_s(k, "sip-password", "");
	c->sip_server = get_s(k, "sip-server", "");
	c->listen = get_b(k, "listen", FALSE);
	c->t38 = get_b(k, "t38", TRUE);
	c->v152 = get_b(k, "v152", TRUE);
	c->v150 = get_b(k, "v150", FALSE);
	c->srtp = get_s(k, "srtp", "");
	c->country = get_s(k, "country", "");
	c->usb = get_s(k, "usb", "auto");
	c->alsa = get_s(k, "alsa", "hw:1");
	c->device = get_s(k, "device", "/dev/ttyDM0");
	c->dmodemd = get_s(k, "dmodemd", "");
	c->init = get_s(k, "init", "");
	c->debug = g_key_file_get_integer(k, "line", "debug", NULL);
	/* the answering machine: off until the user turns it on */
	GError *e = NULL;
	c->voicemail = g_key_file_get_boolean(k, "voice", "voicemail", NULL);
	c->vm_rings = g_key_file_get_integer(k, "voice", "rings", &e);
	if (e || c->vm_rings < 1)
		c->vm_rings = 4;
	g_clear_error(&e);
	c->vm_max = g_key_file_get_integer(k, "voice", "max-seconds", &e);
	if (e || c->vm_max < 10)
		c->vm_max = 120;
	g_clear_error(&e);
	c->remote_proto = g_key_file_get_string(k, "remote", "protocol", NULL);
	if (!c->remote_proto)
		c->remote_proto = g_strdup("SSH");
	c->remote_host = g_key_file_get_string(k, "remote", "host", NULL);
	if (!c->remote_host)
		c->remote_host = g_strdup("");
	c->remote_port = g_key_file_get_integer(k, "remote", "port", NULL);
	c->remote_user = g_key_file_get_string(k, "remote", "user", NULL);
	if (!c->remote_user)
		c->remote_user = g_strdup("");
	c->remote_auth = g_key_file_get_string(k, "remote", "login", NULL);
	if (!c->remote_auth)
		c->remote_auth = g_strdup("auto");
	c->remote_key = g_key_file_get_string(k, "remote", "key", NULL);
	if (!c->remote_key)
		c->remote_key = g_strdup("");
	c->remote_agent = g_key_file_get_string(k, "remote", "agent", NULL);
	if (!c->remote_agent)
		c->remote_agent = g_strdup("");
	c->remote_term = g_key_file_get_string(k, "remote", "terminal", NULL);
	if (!c->remote_term)
		c->remote_term = g_strdup("xterm");
	c->term_type = g_key_file_get_string(k, "terminal", "type", NULL);
	if (!c->term_type)
		c->term_type = g_strdup("xterm");
	g_key_file_unref(k);
}

void config_save(const Config *c)
{
	GKeyFile *k = g_key_file_new();
	g_key_file_set_string(k, "line", "line", c->line);
	g_key_file_set_string(k, "line", "sip-user", c->sip_user);
	g_key_file_set_string(k, "line", "sip-password", c->sip_password);
	g_key_file_set_string(k, "line", "sip-server", c->sip_server);
	g_key_file_set_boolean(k, "line", "listen", c->listen);
	g_key_file_set_boolean(k, "line", "t38", c->t38);
	g_key_file_set_boolean(k, "line", "v152", c->v152);
	g_key_file_set_boolean(k, "line", "v150", c->v150);
	g_key_file_set_string(k, "line", "srtp", c->srtp);
	g_key_file_set_string(k, "line", "country", c->country);
	g_key_file_set_string(k, "line", "usb", c->usb);
	g_key_file_set_string(k, "line", "alsa", c->alsa);
	g_key_file_set_string(k, "line", "device", c->device);
	g_key_file_set_string(k, "line", "dmodemd", c->dmodemd);
	g_key_file_set_string(k, "line", "init", c->init);
	g_key_file_set_integer(k, "line", "debug", c->debug);
	g_key_file_set_boolean(k, "voice", "voicemail", c->voicemail);
	g_key_file_set_integer(k, "voice", "rings", c->vm_rings);
	g_key_file_set_integer(k, "voice", "max-seconds", c->vm_max);
	g_key_file_set_string(k, "remote", "protocol", c->remote_proto);
	g_key_file_set_string(k, "remote", "host", c->remote_host);
	g_key_file_set_integer(k, "remote", "port", c->remote_port);
	g_key_file_set_string(k, "remote", "user", c->remote_user);
	g_key_file_set_string(k, "remote", "login", c->remote_auth);
	g_key_file_set_string(k, "remote", "key", c->remote_key);
	g_key_file_set_string(k, "remote", "agent", c->remote_agent);
	g_key_file_set_string(k, "remote", "terminal", c->remote_term);
	g_key_file_set_string(k, "terminal", "type", c->term_type);
	char *p = path();
	char *dir = g_path_get_dirname(p);
	g_mkdir_with_parents(dir, 0700);
	GError *e = NULL;
	if (!g_key_file_save_to_file(k, p, &e)) {
		g_warning("%s: %s", p, e->message);
		g_error_free(e);
	}
	g_chmod(p, 0600);
	g_free(dir);
	g_free(p);
	g_key_file_unref(k);
}

void config_free(Config *c)
{
	g_free(c->line);
	g_free(c->sip_user);
	g_free(c->sip_password);
	g_free(c->sip_server);
	g_free(c->srtp);
	g_free(c->country);
	g_free(c->usb);
	g_free(c->alsa);
	g_free(c->device);
	g_free(c->dmodemd);
	g_free(c->init);
	g_free(c->remote_proto);
	g_free(c->remote_host);
	g_free(c->remote_user);
	g_free(c->remote_term);
	g_free(c->remote_auth);
	g_free(c->remote_key);
	g_free(c->remote_agent);
	g_free(c->term_type);
}

char *config_summary(const Config *c)
{
	if (!g_strcmp0(c->line, "usb"))
		return g_strdup_printf("USB modem (%s)", c->usb);
	if (!g_strcmp0(c->line, "alsa"))
		return g_strdup_printf("Softmodem codec %s", c->alsa);
	if (!g_strcmp0(c->line, "device"))
		return g_strdup_printf("The modem at %s", c->device);
	if (!*c->sip_user || !*c->sip_server)
		return g_strdup("SIP account not set up");
	return g_strdup_printf("SIP: %s@%s%s", c->sip_user, c->sip_server,
			       c->listen ? ", taking calls" : "");
}

/*
 *    the dialog
 */

static const char *const lines[] = { "sip", "usb", "alsa", "device", NULL };
static const char *const line_labels[] = {
	"SIP account", "USB modem", "Softmodem codec (ALSA)",
	"A modem that already runs", NULL
};
static const char *const srtps[] = { "", "no", "require", NULL };
static const char *const srtp_labels[] = {
	"When the other end offers it", "Never", "Always (refuse plain RTP)",
	NULL
};

typedef struct Prefs {
	App *a;
	AdwComboRow *line, *srtp;
	AdwEntryRow *user, *server, *country, *usb, *alsa, *device, *dmodemd,
		*init;
	AdwPasswordEntryRow *password;
	AdwSwitchRow *listen, *t38, *v152, *v150;
	AdwSpinRow *debug;
	AdwPreferencesGroup *sip, *usb_g, *alsa_g, *device_g;
} Prefs;

static int index_of(const char *const *v, const char *s)
{
	for (int i = 0; v[i]; i++)
		if (!g_strcmp0(v[i], s))
			return i;
	return 0;
}

static void show_groups(Prefs *p)
{
	const char *l = lines[adw_combo_row_get_selected(p->line)];
	gtk_widget_set_visible(GTK_WIDGET(p->sip), !g_strcmp0(l, "sip"));
	gtk_widget_set_visible(GTK_WIDGET(p->usb_g), !g_strcmp0(l, "usb"));
	gtk_widget_set_visible(GTK_WIDGET(p->alsa_g), !g_strcmp0(l, "alsa"));
	gtk_widget_set_visible(GTK_WIDGET(p->device_g),
			       !g_strcmp0(l, "device"));
}

static void line_changed(GObject *o, GParamSpec *ps, gpointer data)
{
	(void)o;
	(void)ps;
	show_groups(data);
}

static AdwEntryRow *entry(AdwPreferencesGroup *g, const char *title,
			  const char *value)
{
	GtkWidget *r = adw_entry_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r), title);
	gtk_editable_set_text(GTK_EDITABLE(r), value);
	adw_preferences_group_add(g, r);
	return ADW_ENTRY_ROW(r);
}

static AdwSwitchRow *toggle(AdwPreferencesGroup *g, const char *title,
			    const char *sub, gboolean on)
{
	GtkWidget *r = adw_switch_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r), title);
	if (sub)
		adw_action_row_set_subtitle(ADW_ACTION_ROW(r), sub);
	adw_switch_row_set_active(ADW_SWITCH_ROW(r), on);
	adw_preferences_group_add(g, r);
	return ADW_SWITCH_ROW(r);
}

static AdwComboRow *combo(AdwPreferencesGroup *g, const char *title,
			  const char *const *labels, int sel)
{
	GtkWidget *r = adw_combo_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r), title);
	GtkStringList *m = gtk_string_list_new((const char *const *)labels);
	adw_combo_row_set_model(ADW_COMBO_ROW(r), G_LIST_MODEL(m));
	g_object_unref(m);
	adw_combo_row_set_selected(ADW_COMBO_ROW(r), (guint)sel);
	adw_preferences_group_add(g, r);
	return ADW_COMBO_ROW(r);
}

static AdwPreferencesGroup *group(AdwPreferencesPage *page, const char *title,
				  const char *desc)
{
	GtkWidget *g = adw_preferences_group_new();
	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g), title);
	if (desc)
		adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(g),
						      desc);
	adw_preferences_page_add(page, ADW_PREFERENCES_GROUP(g));
	return ADW_PREFERENCES_GROUP(g);
}

static char *text_of(AdwEntryRow *r)
{
	return g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(r))));
}

static void closed(AdwDialog *d, gpointer data)
{
	(void)d;
	Prefs *p = data;
	Config *c = &p->a->cfg;
	config_free(c);
	c->line = g_strdup(lines[adw_combo_row_get_selected(p->line)]);
	c->sip_user = text_of(p->user);
	c->sip_password = g_strdup(gtk_editable_get_text(GTK_EDITABLE(p->password)));
	c->sip_server = text_of(p->server);
	c->listen = adw_switch_row_get_active(p->listen);
	c->t38 = adw_switch_row_get_active(p->t38);
	c->v152 = adw_switch_row_get_active(p->v152);
	c->v150 = adw_switch_row_get_active(p->v150);
	c->srtp = g_strdup(srtps[adw_combo_row_get_selected(p->srtp)]);
	c->country = text_of(p->country);
	c->usb = text_of(p->usb);
	c->alsa = text_of(p->alsa);
	c->device = text_of(p->device);
	c->dmodemd = text_of(p->dmodemd);
	c->init = text_of(p->init);
	c->debug = (int)adw_spin_row_get_value(p->debug);
	config_save(c);
	window_state(p->a);
	g_free(p);
}

void prefs_show(App *a)
{
	Config *c = &a->cfg;
	Prefs *p = g_new0(Prefs, 1);
	p->a = a;
	AdwDialog *d = ADW_DIALOG(adw_preferences_dialog_new());
	adw_dialog_set_title(d, "Line");
	AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
	adw_preferences_page_set_title(page, "Line");
	adw_preferences_page_set_icon_name(page, "network-wired-symbolic");

	AdwPreferencesGroup *g = group(page, "The modem's line",
		"Where dmodemd's calls go. Changes take effect the next time "
		"the modem starts.");
	p->line = combo(g, "Line", line_labels, index_of(lines, c->line));
	p->country = entry(g, "Country (dmodemd -c, empty for the USA)",
			   c->country);

	p->sip = group(page, "SIP account", "Calls go to sip:<number>@server.");
	p->user = entry(p->sip, "User name", c->sip_user);
	GtkWidget *pw = adw_password_entry_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(pw), "Password");
	gtk_editable_set_text(GTK_EDITABLE(pw), c->sip_password);
	adw_preferences_group_add(p->sip, pw);
	p->password = ADW_PASSWORD_ENTRY_ROW(pw);
	p->server = entry(p->sip, "Server (host or host:port)", c->sip_server);
	p->listen = toggle(p->sip, "Take calls", "Register, so calls to the "
			   "account ring the modem", c->listen);
	p->t38 = toggle(p->sip, "Fax over T.38", NULL, c->t38);
	p->v152 = toggle(p->sip, "Mark calls as modem data (V.152)", NULL,
			 c->v152);
	p->v150 = toggle(p->sip, "Modem relay (V.150.1)", NULL, c->v150);
	p->srtp = combo(p->sip, "Encrypted media (SRTP)", srtp_labels,
			index_of(srtps, c->srtp));

	p->usb_g = group(page, "USB modem", "A SmartUSB56, a pico-serial "
			 "Line or WB, an Agere or an Apple USB modem.");
	p->usb = entry(p->usb_g, "Device (auto, or bus:address)", c->usb);
	p->alsa_g = group(page, "Softmodem codec", "An AC'97 or HD-audio "
			  "modem codec through ALSA (aplay -l lists it).");
	p->alsa = entry(p->alsa_g, "ALSA device", c->alsa);
	p->device_g = group(page, "A modem that already runs",
			    "dmodemd started as a service, or any modem's "
			    "serial port.");
	p->device = entry(p->device_g, "Device", c->device);

	AdwPreferencesGroup *adv = group(page, "Advanced", NULL);
	p->init = entry(adv, "Commands when the modem opens", c->init);
	p->dmodemd = entry(adv, "dmodemd (empty to look for it)", c->dmodemd);
	GtkWidget *dbg = adw_spin_row_new_with_range(0, 9, 1);
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dbg),
				      "Debug messages (dmodemd -d)");
	adw_spin_row_set_value(ADW_SPIN_ROW(dbg), c->debug);
	adw_preferences_group_add(adv, dbg);
	p->debug = ADW_SPIN_ROW(dbg);

	show_groups(p);
	g_signal_connect(p->line, "notify::selected", G_CALLBACK(line_changed),
			 p);
	adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(d), page);
	g_signal_connect(d, "closed", G_CALLBACK(closed), p);
	adw_dialog_present(d, GTK_WIDGET(a->window));
}
