/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * ports.cpp - the serial ports of this computer and the modems on them,
 *             for the Line Settings dialog ("Look for a modem").
 *
 *   - Windows: the modems that Windows knows (the Modem class in the
 *     registry, AttachedTo), the MultiTech USB modems (Enum\USB, NT),
 *     and the ports of HARDWARE\DEVICEMAP\SERIALCOMM. If the registry
 *     has no ports, the app tries COM1 to COM16.
 *   - Linux (winport/): /sys/class/tty (ttyUSB and ttyACM), with the
 *     USB maker and product of each port. A MultiTech MT9234MU with no
 *     firmware has no port: ports_problem() tells you what to do.
 *   - macOS (winport/): the /dev/cu.* devices.
 */
#include "app.h"

#include <stdlib.h>

#ifdef DMODEM_WINPORT
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* The MultiTech modems with a TI TUSB3410 USB UART (vendor 06E0) */
static const struct {
	const char *product, *name;
} multitech[] = {
	{ "f114", "MultiTech MT9234MU" },
	{ "f115", "MultiTech MT9234ZBA-USB" },
	{ "0319", "MultiTech MT9234ZBA-USB" },
};
#define N_MULTITECH (int)(sizeof(multitech) / sizeof(multitech[0]))

/* Copies at most n - 1 characters of src to dst, and ends dst with a
   NUL. */
static void copy(char *dst, const char *src, int n)
{
	if (n <= 0)
		return;
	strncpy(dst, src ? src : "", (size_t)n - 1);
	dst[n - 1] = '\0';
}

/* Gives the index of the port with this name in list, or -1. */
static int find(const PortInfo *list, int count, const char *name)
{
	for (int i = 0; i < count; i++)
		if (!_stricmp(list[i].name, name))
			return i;
	return -1;
}

/* Adds a port to list (or changes the port with the same name). A
   description replaces an empty one. modem marks the port as a modem.
   Gives the new count. */
static int add(PortInfo *list, int count, int max, const char *name,
	       const char *desc, int modem)
{
	if (!name || !*name)
		return count;
	int i = find(list, count, name);
	if (i < 0) {
		if (count >= max)
			return count;
		i = count++;
		memset(&list[i], 0, sizeof(list[i]));
		copy(list[i].name, name, sizeof(list[i].name));
	}
	if (desc && *desc && (!list[i].desc[0] || modem))
		copy(list[i].desc, desc, sizeof(list[i].desc));
	if (modem)
		list[i].modem = 1;
	return count;
}

/* Puts the modems first, and keeps the order of the other ports. */
static void modems_first(PortInfo *list, int count)
{
	for (int i = 1; i < count; i++) {
		if (!list[i].modem)
			continue;
		PortInfo p = list[i];
		int j = i;
		while (j > 0 && !list[j - 1].modem) {
			list[j] = list[j - 1];
			j--;
		}
		list[j] = p;
	}
}

#ifndef DMODEM_WINPORT

/* Reads the string value name of key k into out (n bytes). Gives true
   if the value is there. */
static bool reg_string(HKEY k, const char *name, char *out, int n)
{
	DWORD type = 0, size = (DWORD)n;
	out[0] = '\0';
	if (RegQueryValueEx(k, name, NULL, &type, (BYTE *)out, &size) !=
	    ERROR_SUCCESS || type != REG_SZ)
		return false;
	out[n - 1] = '\0';
	return true;
}

/* Adds the modems of the Modem class under root\path: each subkey that
   has AttachedTo (the port) and FriendlyName or DriverDesc (the
   name). Windows 95, 98 and ME keep the class in
   System\CurrentControlSet\Services\Class\Modem, and NT in
   SYSTEM\CurrentControlSet\Control\Class\{4D36E96D-...}. */
static int class_modems(PortInfo *list, int count, int max, const char *path)
{
	HKEY cls;
	if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &cls) !=
	    ERROR_SUCCESS)
		return count;
	char sub[64];
	for (DWORD i = 0;; i++) {
		DWORD len = sizeof(sub);
		if (RegEnumKeyEx(cls, i, sub, &len, NULL, NULL, NULL, NULL) !=
		    ERROR_SUCCESS)
			break;
		HKEY k;
		if (RegOpenKeyEx(cls, sub, 0, KEY_READ, &k) != ERROR_SUCCESS)
			continue;
		char port[64], name[128];
		if (reg_string(k, "AttachedTo", port, sizeof(port)) &&
		    !strncmp(port, "COM", 3)) {
			if (!reg_string(k, "FriendlyName", name, sizeof(name)))
				reg_string(k, "DriverDesc", name, sizeof(name));
			count = add(list, count, max, port, name, 1);
		}
		RegCloseKey(k);
	}
	RegCloseKey(cls);
	return count;
}

/* Adds the MultiTech USB modems that NT knows:
   Enum\USB\VID_06E0&PID_xxxx\<instance>\Device Parameters, PortName. */
static int usb_modems(PortInfo *list, int count, int max)
{
	for (int m = 0; m < N_MULTITECH; m++) {
		char path[160];
		_snprintf(path, sizeof(path),
			  "SYSTEM\\CurrentControlSet\\Enum\\USB\\VID_06E0&PID_%s",
			  multitech[m].product);
		path[sizeof(path) - 1] = '\0';
		HKEY dev;
		if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &dev) !=
		    ERROR_SUCCESS)
			continue;
		char inst[128];
		for (DWORD i = 0;; i++) {
			DWORD len = sizeof(inst);
			if (RegEnumKeyEx(dev, i, inst, &len, NULL, NULL, NULL,
					 NULL) != ERROR_SUCCESS)
				break;
			char sub[200];
			_snprintf(sub, sizeof(sub), "%s\\Device Parameters", inst);
			sub[sizeof(sub) - 1] = '\0';
			HKEY k;
			if (RegOpenKeyEx(dev, sub, 0, KEY_READ, &k) != ERROR_SUCCESS)
				continue;
			char port[64];
			if (reg_string(k, "PortName", port, sizeof(port)))
				count = add(list, count, max, port,
					    multitech[m].name, 1);
			RegCloseKey(k);
		}
		RegCloseKey(dev);
	}
	return count;
}

/* Adds the ports of HARDWARE\DEVICEMAP\SERIALCOMM (the value data is
   the port name). Gives the new count. */
static int devicemap_ports(PortInfo *list, int count, int max)
{
	HKEY k;
	if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM",
			 0, KEY_READ, &k) != ERROR_SUCCESS)
		return count;
	for (DWORD i = 0;; i++) {
		char name[128], data[64];
		DWORD nlen = sizeof(name), dlen = sizeof(data), type = 0;
		if (RegEnumValue(k, i, name, &nlen, NULL, &type, (BYTE *)data,
				 &dlen) != ERROR_SUCCESS)
			break;
		if (type != REG_SZ)
			continue;
		data[sizeof(data) - 1] = '\0';
		count = add(list, count, max, data, NULL, 0);
	}
	RegCloseKey(k);
	return count;
}

/* Adds COM1 to COM16 when they open, or when another program has them
   open (access denied). For a Windows with no SERIALCOMM key. */
static int probe_ports(PortInfo *list, int count, int max)
{
	for (int i = 1; i <= 16; i++) {
		char name[8], path[16];
		_snprintf(name, sizeof(name), "COM%d", i);
		name[sizeof(name) - 1] = '\0';
		_snprintf(path, sizeof(path), "\\\\.\\%s", name);
		path[sizeof(path) - 1] = '\0';
		HANDLE h = CreateFile(path, GENERIC_READ | GENERIC_WRITE, 0,
				      NULL, OPEN_EXISTING, 0, NULL);
		if (h != INVALID_HANDLE_VALUE) {
			CloseHandle(h);
			count = add(list, count, max, name, NULL, 0);
		} else if (GetLastError() == ERROR_ACCESS_DENIED) {
			count = add(list, count, max, name, "in use", 0);
		}
	}
	return count;
}

int ports_list(PortInfo *out, int max)
{
	int count = 0;
	count = class_modems(out, count, max,
			     "SYSTEM\\CurrentControlSet\\Control\\Class\\"
			     "{4D36E96D-E325-11CE-BFC1-08002BE10318}");
	count = class_modems(out, count, max,
			     "System\\CurrentControlSet\\Services\\Class\\Modem");
	count = usb_modems(out, count, max);
	int known = count;
	count = devicemap_ports(out, count, max);
	if (count == known)
		count = probe_ports(out, count, max);
	modems_first(out, count);
	return count;
}

void ports_problem(char *out, int n)
{
	if (n > 0)
		out[0] = '\0';
}

#elif defined(__APPLE__)

int ports_list(PortInfo *out, int max)
{
	int count = 0;
	DIR *d = opendir("/dev");
	if (!d)
		return 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		const char *n = e->d_name;
		if (strncmp(n, "cu.", 3) || strstr(n, "Bluetooth") ||
		    strstr(n, "debug-console"))
			continue;
		char path[64];
		_snprintf(path, sizeof(path), "/dev/%s", n);
		path[sizeof(path) - 1] = '\0';
		/* (CDC-ACM modems are cu.usbmodem*: the MT9234MU-CDC
		   among them) */
		int modem = !strncmp(n, "cu.usbmodem", 11);
		count = add(out, count, max, path,
			    modem ? "USB modem" :
			    !strncmp(n, "cu.usbserial", 12) ? "USB serial port" :
			    NULL, modem);
	}
	closedir(d);
	modems_first(out, count);
	return count;
}

void ports_problem(char *out, int n)
{
	if (n > 0)
		out[0] = '\0';
}

#else /* Linux */

/* the root of sysfs (a test can give another one) */
#ifndef PORTS_SYSFS
#define PORTS_SYSFS "/sys"
#endif

/* Reads the first line of the sysfs file dir/name into out (n bytes),
   with no space at the end. Gives true if the file is there. */
static bool attr(const char *dir, const char *name, char *out, int n)
{
	char path[PATH_MAX];
	_snprintf(path, sizeof(path), "%s/%s", dir, name);
	path[sizeof(path) - 1] = '\0';
	out[0] = '\0';
	FILE *f = fopen(path, "r");
	if (!f)
		return false;
	if (!fgets(out, n, f))
		out[0] = '\0';
	fclose(f);
	size_t l = strlen(out);
	while (l > 0 && (unsigned char)out[l - 1] <= ' ')
		out[--l] = '\0';
	return true;
}

/* Gives the name of the MultiTech model for USB product pid (vendor
   06E0), or NULL. */
static const char *multitech_name(const char *vid, const char *pid)
{
	if (_stricmp(vid, "06e0"))
		return NULL;
	for (int m = 0; m < N_MULTITECH; m++)
		if (!_stricmp(pid, multitech[m].product))
			return multitech[m].name;
	return NULL;
}

/* Tells if the text has "modem" in it, in upper or lower case. */
static bool says_modem(const char *s)
{
	for (; *s; s++)
		if (!_strnicmp(s, "modem", 5))
			return true;
	return false;
}

int ports_list(PortInfo *out, int max)
{
	int count = 0;
	DIR *d = opendir(PORTS_SYSFS "/class/tty");
	if (!d)
		return 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		const char *n = e->d_name;
		if (strncmp(n, "ttyUSB", 6) && strncmp(n, "ttyACM", 6))
			continue;
		char link[PATH_MAX], itf[PATH_MAX], usb[PATH_MAX];
		_snprintf(link, sizeof(link), PORTS_SYSFS "/class/tty/%s/device", n);
		link[sizeof(link) - 1] = '\0';
		char desc[128] = "";
		int modem = 0;
		/* (device is the USB interface; its parent is the USB
		   device, with idVendor, idProduct, manufacturer and
		   product) */
		if (realpath(link, itf)) {
			_snprintf(usb, sizeof(usb), "%s/..", itf);
			usb[sizeof(usb) - 1] = '\0';
			char vid[16], pid[16], maker[64], product[64];
			attr(usb, "idVendor", vid, sizeof(vid));
			attr(usb, "idProduct", pid, sizeof(pid));
			attr(usb, "manufacturer", maker, sizeof(maker));
			attr(usb, "product", product, sizeof(product));
			const char *mt = multitech_name(vid, pid);
			if (mt) {
				copy(desc, mt, sizeof(desc));
				modem = 1;
			} else if (maker[0] || product[0]) {
				_snprintf(desc, sizeof(desc), "%s%s%s", maker,
					  maker[0] && product[0] ? " " : "",
					  product);
				desc[sizeof(desc) - 1] = '\0';
				modem = says_modem(desc);
			}
		}
		char path[64];
		_snprintf(path, sizeof(path), "/dev/%s", n);
		path[sizeof(path) - 1] = '\0';
		count = add(out, count, max, path, desc, modem);
	}
	closedir(d);
	modems_first(out, count);
	return count;
}

/* The places where Linux looks for the firmware of the MT9234MU */
static const char *const firmware[] = {
	"/lib/firmware/updates/mts_mt9234mu.fw",
	"/lib/firmware/mts_mt9234mu.fw",
	"/usr/lib/firmware/updates/mts_mt9234mu.fw",
	"/usr/lib/firmware/mts_mt9234mu.fw",
	"/lib/firmware/mts_mt9234zba.fw",
	"/usr/lib/firmware/mts_mt9234zba.fw",
	NULL
};

/* Tells if Linux has the firmware of the MT9234MU. */
static bool firmware_present()
{
	struct stat st;
	for (int i = 0; firmware[i]; i++)
		if (stat(firmware[i], &st) == 0)
			return true;
	return false;
}

/* Gives true if USB device dir has a tty in one of its interfaces
   ("1-2:1.0/ttyUSB0", or "1-2:1.0/tty/ttyACM0"). */
static bool has_tty(const char *dir)
{
	DIR *d = opendir(dir);
	if (!d)
		return false;
	bool found = false;
	struct dirent *e;
	while (!found && (e = readdir(d)) != NULL) {
		if (!strchr(e->d_name, ':'))
			continue;
		char itf[PATH_MAX];
		_snprintf(itf, sizeof(itf), "%s/%s", dir, e->d_name);
		itf[sizeof(itf) - 1] = '\0';
		DIR *i = opendir(itf);
		struct dirent *t;
		while (i && !found && (t = readdir(i)) != NULL)
			if (!strncmp(t->d_name, "tty", 3))
				found = true;
		if (i)
			closedir(i);
	}
	closedir(d);
	return found;
}

void ports_problem(char *out, int n)
{
	if (n <= 0)
		return;
	out[0] = '\0';
	DIR *d = opendir(PORTS_SYSFS "/bus/usb/devices");
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		char dir[PATH_MAX], vid[16], pid[16], nc[8];
		_snprintf(dir, sizeof(dir), PORTS_SYSFS "/bus/usb/devices/%s", e->d_name);
		dir[sizeof(dir) - 1] = '\0';
		if (!attr(dir, "idVendor", vid, sizeof(vid)) ||
		    !attr(dir, "idProduct", pid, sizeof(pid)))
			continue;
		const char *mt = multitech_name(vid, pid);
		if (!mt || has_tty(dir))
			continue;
		attr(dir, "bNumConfigurations", nc, sizeof(nc));
		/* (one configuration: the boot ROM of the TUSB3410, with no
		   firmware loaded) */
		bool no_fw = !strcmp(nc, "1");
		const char *to = strstr(mt, "ZBA") ?
			"/lib/firmware/mts_mt9234zba.fw" : "/lib/firmware/";
		if (no_fw && !firmware_present())
			_snprintf(out, (size_t)n,
				  "%s: no firmware. Copy mts_mt9234mu.fw "
				  "(share/dmodem-gui/firmware) to %s, then "
				  "connect the modem again.", mt, to);
		else if (no_fw)
			_snprintf(out, (size_t)n,
				  "%s: no firmware. Load the ti_usb_3410_5052 "
				  "driver, then connect the modem again.", mt);
		else
			_snprintf(out, (size_t)n,
				  "%s: no serial port. Load the "
				  "ti_usb_3410_5052 driver.", mt);
		out[n - 1] = '\0';
		break;
	}
	closedir(d);
}

#endif
