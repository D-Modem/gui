/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * usbmodem.c - hardware modems on USB that the "device" line can open:
 *              the MultiTech MT9234MU and MT9234ZBA-USB, found in sysfs
 *
 * These modems have a TI TUSB3410 USB UART.  Linux's ti_usb_3410_5052
 * driver downloads its firmware (mts_mt9234mu.fw) into the chip; then
 * the modem is a ttyUSB.  Without the firmware file the modem stays in
 * the chip's boot ROM (one configuration) and has no tty.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "app.h"

/* MultiTech's vendor ID and the products with a TUSB3410 */
static const struct {
	const char *product, *name;
} models[] = {
	{ "f114", "MultiTech MT9234MU" },
	{ "f115", "MultiTech MT9234ZBA-USB" },
	{ "0319", "MultiTech MT9234ZBA-USB" },
};

/* the firmware file's places that Linux looks in */
static const char *const firmware[] = {
	"/lib/firmware/updates/mts_mt9234mu.fw",
	"/lib/firmware/mts_mt9234mu.fw",
	"/usr/lib/firmware/updates/mts_mt9234mu.fw",
	"/usr/lib/firmware/mts_mt9234mu.fw",
	NULL
};

/* the trimmed text of sysfs file dir/name, or NULL */
static char *attr(const char *dir, const char *name)
{
	char *path = g_build_filename(dir, name, NULL);
	char *t = NULL;
	if (g_file_get_contents(path, &t, NULL, NULL))
		g_strstrip(t);
	g_free(path);
	return t;
}

/* the first ttyUSB under USB device dir's interfaces, as /dev/ttyUSBn,
   or NULL */
static char *tty_of(const char *dir)
{
	char *found = NULL;
	GDir *d = g_dir_open(dir, 0, NULL);
	if (!d)
		return NULL;
	const char *e;
	while (!found && (e = g_dir_read_name(d))) {
		/* (its interfaces: "1-2:2.0") */
		if (!strchr(e, ':'))
			continue;
		char *itf = g_build_filename(dir, e, NULL);
		GDir *i = g_dir_open(itf, 0, NULL);
		const char *t;
		while (i && !found && (t = g_dir_read_name(i)))
			if (g_str_has_prefix(t, "ttyUSB"))
				found = g_strconcat("/dev/", t, NULL);
		if (i)
			g_dir_close(i);
		g_free(itf);
	}
	g_dir_close(d);
	return found;
}

gboolean usbmodem_firmware_present(void)
{
	for (int i = 0; firmware[i]; i++)
		if (g_file_test(firmware[i], G_FILE_TEST_EXISTS))
			return TRUE;
	return FALSE;
}

UsbModem *usbmodem_find(void)
{
	const char *root = "/sys/bus/usb/devices";
	GDir *d = g_dir_open(root, 0, NULL);
	if (!d)
		return NULL;
	UsbModem *m = NULL;
	const char *e;
	while (!m && (e = g_dir_read_name(d))) {
		char *dir = g_build_filename(root, e, NULL);
		char *vid = attr(dir, "idVendor");
		char *pid = vid ? attr(dir, "idProduct") : NULL;
		for (size_t k = 0; pid && !m &&
		     k < sizeof(models) / sizeof(models[0]); k++) {
			if (g_ascii_strcasecmp(vid, "06e0") ||
			    g_ascii_strcasecmp(pid, models[k].product))
				continue;
			m = g_new0(UsbModem, 1);
			m->name = g_strdup(models[k].name);
			m->tty = tty_of(dir);
			char *nc = attr(dir, "bNumConfigurations");
			/* (one configuration: the boot ROM's) */
			m->no_firmware = !m->tty && nc && !strcmp(nc, "1");
			g_free(nc);
		}
		g_free(vid);
		g_free(pid);
		g_free(dir);
	}
	g_dir_close(d);
	return m;
}

char *usbmodem_describe(const UsbModem *m)
{
	if (m->tty)
		return g_strdup_printf("%s at %s", m->name, m->tty);
	if (m->no_firmware && !usbmodem_firmware_present())
		return g_strdup_printf("%s, with no firmware: put "
				       "mts_mt9234mu.fw in /lib/firmware, "
				       "then connect the modem again", m->name);
	if (m->no_firmware)
		return g_strdup_printf("%s, with no firmware: load the "
				       "ti_usb_3410_5052 driver, then connect "
				       "the modem again", m->name);
	return g_strdup_printf("%s, with no serial port: load the "
			       "ti_usb_3410_5052 driver", m->name);
}

gboolean usbmodem_is(const char *path)
{
	UsbModem *m = usbmodem_find();
	gboolean is = m && m->tty && path && !strcmp(m->tty, path);
	usbmodem_free(m);
	return is;
}

void usbmodem_free(UsbModem *m)
{
	if (!m)
		return;
	g_free(m->name);
	g_free(m->tty);
	g_free(m);
}
