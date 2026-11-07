/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * diag.c - the last call's statistics from AT#UD (dmodemctl.h)
 *
 * AT#UD follows Microsoft's Unimodem Diagnostics Command Reference
 * Specification, version 1.0 (D-Modem's docs/specs/umud10.rtf): lines
 * "DIAG <2A4D3263 key=value ...>", the keys and values in hexadecimal
 * and the code tables V.58's.  Each key becomes a row with its label
 * and its value for people.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dmodemctl.h"
#include "util.h"

/* how a key's value reads */
enum fmt {
	F_HEX,          /* as it is */
	F_NUM,          /* a count */
	F_BPS,          /* bit/s */
	F_DBM,          /* -dBm */
	F_DB,           /* dB */
	F_MS,           /* ms */
	F_HZ,           /* Hz */
	F_BAUD,         /* symbols a second */
	F_BYTES,        /* bytes */
	F_TABLE,        /* one of the code tables */
	F_STRING,       /* quoted octets */
	F_VERSION       /* digit.digit */
};

struct code {
	unsigned v;
	const char *label;
};

static const struct code setup[] = {
	{ 0x0, "No previous call" },
	{ 0x1, "No dial tone" },
	{ 0x2, "Network busy (reorder)" },
	{ 0x3, "Busy" },
	{ 0x4, "No signal recognised" },
	{ 0x5, "Voice" },
	{ 0x6, "Text telephone" },
	{ 0x7, "Data answer tone heard" },
	{ 0x8, "Data calling tone heard" },
	{ 0x9, "Fax answer tone heard" },
	{ 0xA, "Fax calling tone heard" },
	{ 0xB, "V.8bis" },
	{ 0, NULL }
};

static const struct code media[] = {
	{ 0x0, "Data" },
	{ 0x1, "Fax" },
	{ 0x2, "Voice" },
	{ 0x3, "VoiceView" },
	{ 0x4, "ASVD (V.61)" },
	{ 0x5, "ASVD (V.34Q)" },
	{ 0x6, "DSVD (Multi-Tech)" },
	{ 0x7, "DSVD 1.2" },
	{ 0x8, "DSVD (V.70)" },
	{ 0x9, "Video telephony (H.324)" },
	{ 0xA, "Other V.80 call" },
	{ 0, NULL }
};

static const struct code dte_mode[] = {
	{ 0x0, "Asynchronous" },
	{ 0x1, "V.80 transparent synchronous" },
	{ 0x2, "V.80 framed synchronous" },
	{ 0, NULL }
};

/* V.58's gstnModulationSchemeActive, and the manufacturers' */
static const struct code modulation[] = {
	{ 0x0, "V.17" }, { 0x1, "V.21" }, { 0x2, "V.22" },
	{ 0x3, "V.22bis" }, { 0x4, "V.23" }, { 0x5, "V.23 (half duplex)" },
	{ 0x6, "V.26bis" }, { 0x7, "V.26ter" }, { 0x8, "V.27ter" },
	{ 0x9, "V.29" }, { 0xA, "V.32" }, { 0xB, "V.32bis" },
	{ 0xC, "V.34" }, { 0xD, "V.34 half duplex" },
	{ 0xE, "V.90 (PCM)" }, { 0xF, "V.91 (PCM both ways)" },
	{ 0x80, "X2" }, { 0x81, "K56flex" }, { 0x82, "V.FC" },
	{ 0x83, "V.32terbo" }, { 0x84, "Bell 212A" }, { 0x85, "Bell 103" },
	{ 0, NULL }
};

static const struct code error_control[] = {
	{ 0x0, "None" },
	{ 0x1, "V.42 LAPM" },
	{ 0x2, "MNP" },
	{ 0x80, "MNP 10" },
	{ 0x81, "ECP" },
	{ 0x82, "ETC" },
	{ 0, NULL }
};

static const struct code compression[] = {
	{ 0x0, "None" },
	{ 0x1, "V.42bis" },
	{ 0x80, "MNP 5" },
	{ 0x81, "V.44" },
	{ 0x82, "zstd" },
	{ 0, NULL }
};

static const struct code flow[] = {
	{ 0x0, "Off" },
	{ 0x1, "XON/XOFF" },
	{ 0x2, "RTS/CTS" },
	{ 0, NULL }
};

/* V.58's callCleared */
static const struct code cleared[] = {
	{ 0x0, "Cause unknown" },
	{ 0x1, "No previous call" },
	{ 0x2, "Call in progress" },
	{ 0x3, "Call waiting signal" },
	{ 0x4, "Delayed" },
	{ 0x19, "Inactivity timer" },
	{ 0x1F, "DTR off stops the dial" },
	{ 0x20, "DTR turned off" },
	{ 0x28, "No number" },
	{ 0x29, "Blacklisted number" },
	{ 0x2A, "Too many attempts" },
	{ 0x2B, "Extension off hook" },
	{ 0x2C, "No answer in time (S7)" },
	{ 0x2D, "Incoming call while dialling" },
	{ 0x2E, "Loop current lost" },
	{ 0x2F, "No dial tone" },
	{ 0x30, "Voice" },
	{ 0x31, "Reorder tone" },
	{ 0x32, "SIT tone" },
	{ 0x33, "Busy" },
	{ 0x34, "Long space" },
	{ 0x3C, "Carrier lost" },
	{ 0x3D, "Training failed" },
	{ 0x3E, "No modulation in common" },
	{ 0x3F, "Retrain failed" },
	{ 0x40, "Too many retrains" },
	{ 0x41, "The other modem hung up" },
	{ 0x42, "Fax detected" },
	{ 0x50, "Stopped by a key" },
	{ 0x51, "Hung up (ATH)" },
	{ 0x52, "Reset (ATZ)" },
	{ 0x5A, "Frame reject" },
	{ 0x5B, "No error control" },
	{ 0x5C, "Protocol violation" },
	{ 0x5D, "Too many retransmissions" },
	{ 0x5E, "Negotiation failed" },
	{ 0x5F, "Disconnect frame received" },
	{ 0x60, "SABME frame received" },
	{ 0x64, "Lost synchronisation" },
	{ 0, NULL }
};

struct keyinfo {
	int key;
	const char *label;
	int fmt;
	const struct code *table;
};

static const struct keyinfo keys[] = {
	{ 0x00, "Report version", F_VERSION, NULL },
	{ 0x01, "Call setup", F_TABLE, setup },
	{ 0x02, "Mode", F_TABLE, media },
	{ 0x03, "DTE interface", F_TABLE, dte_mode },
	{ 0x04, "V.8 CM", F_STRING, NULL },
	{ 0x05, "V.8 JM", F_STRING, NULL },
	{ 0x10, "Received level", F_DBM, NULL },
	{ 0x11, "Transmit level", F_DBM, NULL },
	{ 0x12, "Noise level", F_DBM, NULL },
	{ 0x13, "Mean squared error", F_NUM, NULL },
	{ 0x14, "Near echo loss", F_DB, NULL },
	{ 0x15, "Far echo loss", F_DB, NULL },
	{ 0x16, "Far echo delay", F_MS, NULL },
	{ 0x17, "Round trip delay", F_MS, NULL },
	{ 0x18, "V.34 INFO bits", F_HEX, NULL },
	{ 0x20, "Transmit modulation", F_TABLE, modulation },
	{ 0x21, "Receive modulation", F_TABLE, modulation },
	{ 0x22, "Transmit symbol rate", F_BAUD, NULL },
	{ 0x23, "Receive symbol rate", F_BAUD, NULL },
	{ 0x24, "Transmit carrier", F_HZ, NULL },
	{ 0x25, "Receive carrier", F_HZ, NULL },
	{ 0x26, "First transmit rate", F_BPS, NULL },
	{ 0x27, "First receive rate", F_BPS, NULL },
	{ 0x30, "Carrier losses", F_NUM, NULL },
	{ 0x31, "Rate renegotiations", F_NUM, NULL },
	{ 0x32, "Retrains asked for", F_NUM, NULL },
	{ 0x33, "Retrains granted", F_NUM, NULL },
	{ 0x34, "Last transmit rate", F_BPS, NULL },
	{ 0x35, "Last receive rate", F_BPS, NULL },
	{ 0x40, "Error control", F_TABLE, error_control },
	{ 0x41, "Frame size", F_BYTES, NULL },
	{ 0x42, "Link timeouts", F_NUM, NULL },
	{ 0x43, "NAKs received", F_NUM, NULL },
	{ 0x44, "Compression", F_TABLE, compression },
	{ 0x45, "Dictionary size", F_BYTES, NULL },
	{ 0x50, "Transmit flow control", F_TABLE, flow },
	{ 0x51, "Receive flow control", F_TABLE, flow },
	{ 0x52, "Characters sent", F_NUM, NULL },
	{ 0x53, "Characters received", F_NUM, NULL },
	{ 0x54, "Characters lost sending", F_NUM, NULL },
	{ 0x55, "Characters lost receiving", F_NUM, NULL },
	{ 0x56, "Frames sent", F_NUM, NULL },
	{ 0x57, "Frames received", F_NUM, NULL },
	{ 0x58, "Frames sent again", F_NUM, NULL },
	{ 0x59, "Frames received in error", F_NUM, NULL },
	{ 0x60, "Why the call ended", F_TABLE, cleared },
	{ 0x61, "Call waiting events", F_NUM, NULL },
	{ -1, NULL, 0, NULL }
};

struct row {
	int key;
	char *raw, *label, *value;
};

struct dmc_diag {
	struct row *rows;
	int n;
};

static const struct keyinfo *info(int key)
{
	for (const struct keyinfo *k = keys; k->label; k++)
		if (k->key == key)
			return k;
	return NULL;
}

static const char *code_label(const struct code *t, unsigned v)
{
	for (; t->label; t++)
		if (t->v == v)
			return t->label;
	return NULL;
}

/* a number with thousands apart by commas */
static void grouped(char *out, size_t n, unsigned long v)
{
	char digits[32];
	snprintf(digits, sizeof(digits), "%lu", v);
	size_t len = strlen(digits), o = 0;
	for (size_t i = 0; i < len && o + 2 < n; i++) {
		if (i && (len - i) % 3 == 0)
			out[o++] = ',';
		out[o++] = digits[i];
	}
	out[o] = '\0';
}

static char *shown(const struct keyinfo *k, const char *raw)
{
	char out[160];
	unsigned long v = strtoul(raw, NULL, 16);
	char num[40];
	grouped(num, sizeof(num), v);
	switch (k ? k->fmt : F_HEX) {
	case F_NUM:
		snprintf(out, sizeof(out), "%s", num);
		break;
	case F_BPS:
		snprintf(out, sizeof(out), "%s bit/s", num);
		break;
	case F_DBM:
		snprintf(out, sizeof(out), "%s%lu dBm", v ? "-" : "", v);
		break;
	case F_DB:
		snprintf(out, sizeof(out), "%lu dB", v);
		break;
	case F_MS:
		snprintf(out, sizeof(out), "%lu ms", v);
		break;
	case F_HZ:
		snprintf(out, sizeof(out), "%lu Hz", v);
		break;
	case F_BAUD:
		snprintf(out, sizeof(out), "%lu baud", v);
		break;
	case F_BYTES:
		snprintf(out, sizeof(out), "%s bytes", num);
		break;
	case F_TABLE: {
		const char *l = code_label(k->table, (unsigned)v);
		if (l)
			snprintf(out, sizeof(out), "%s", l);
		else
			snprintf(out, sizeof(out), "Code %lX", v);
		break;
	}
	case F_VERSION:
		if (strlen(raw) == 2)
			snprintf(out, sizeof(out), "%c.%c", raw[0], raw[1]);
		else
			snprintf(out, sizeof(out), "%s", raw);
		break;
	case F_STRING: {
		/* the octets, apart by spaces */
		size_t o = 0;
		for (const char *p = raw; *p && o + 4 < sizeof(out); p++) {
			if (*p == '"')
				continue;
			if (o && (o + 1) % 3 == 0)
				out[o++] = ' ';
			out[o++] = *p;
		}
		out[o] = '\0';
		break;
	}
	default:
		snprintf(out, sizeof(out), "%s", raw);
		break;
	}
	return dup_str(out);
}

static void add(dmc_diag *d, int key, const char *raw, size_t n)
{
	/* a key again: its last value */
	for (int i = 0; i < d->n; i++)
		if (d->rows[i].key == key) {
			free(d->rows[i].raw);
			free(d->rows[i].value);
			d->rows[i].raw = dup_n(raw, n);
			d->rows[i].value = shown(info(key), d->rows[i].raw);
			return;
		}
	struct row *r = realloc(d->rows, sizeof(*r) * (size_t)(d->n + 1));
	if (!r)
		abort();
	d->rows = r;
	r = &d->rows[d->n++];
	const struct keyinfo *k = info(key);
	char label[32];
	if (!k)
		snprintf(label, sizeof(label), "Key %X", key);
	r->key = key;
	r->raw = dup_n(raw, n);
	r->label = dup_str(k ? k->label : label);
	r->value = shown(k, r->raw);
}

dmc_diag *dmc_diag_parse(const char *text)
{
	dmc_diag *d = calloc(1, sizeof(*d));
	if (!d)
		abort();
	const char *p = text ? text : "";
	while ((p = strstr(p, "DIAG <")) != NULL) {
		p += 6;
		const char *end = strchr(p, '>');
		if (!end)
			break;
		/* the tag (2A4D3263), then key=value pairs */
		while (p < end && *p != ' ')
			p++;
		while (p < end) {
			while (p < end && *p == ' ')
				p++;
			const char *eq = p;
			while (eq < end && *eq != '=')
				eq++;
			if (eq >= end)
				break;
			int key = (int)strtol(p, NULL, 16);
			const char *v = eq + 1, *ve = v;
			if (*v == '"') {
				ve = strchr(v + 1, '"');
				ve = ve && ve < end ? ve + 1 : end;
			} else {
				while (ve < end && *ve != ' ')
					ve++;
			}
			add(d, key, v, (size_t)(ve - v));
			p = ve;
		}
		p = end + 1;
	}
	return d;
}

void dmc_diag_free(dmc_diag *d)
{
	if (!d)
		return;
	for (int i = 0; i < d->n; i++) {
		free(d->rows[i].raw);
		free(d->rows[i].label);
		free(d->rows[i].value);
	}
	free(d->rows);
	free(d);
}

int dmc_diag_count(const dmc_diag *d)
{
	return d ? d->n : 0;
}

int dmc_diag_key(const dmc_diag *d, int i)
{
	return d && i >= 0 && i < d->n ? d->rows[i].key : -1;
}

const char *dmc_diag_label(const dmc_diag *d, int i)
{
	return d && i >= 0 && i < d->n ? d->rows[i].label : "";
}

const char *dmc_diag_value(const dmc_diag *d, int i)
{
	return d && i >= 0 && i < d->n ? d->rows[i].value : "";
}

const char *dmc_diag_raw(const dmc_diag *d, int key)
{
	for (int i = 0; d && i < d->n; i++)
		if (d->rows[i].key == key)
			return d->rows[i].raw;
	return NULL;
}

int dmc_diag_has_call(const dmc_diag *d)
{
	const char *v = dmc_diag_raw(d, 1);
	return v && strtoul(v, NULL, 16) != 0;
}
