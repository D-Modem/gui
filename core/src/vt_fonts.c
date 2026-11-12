/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * vt_fonts.c - the terminal types' fonts (the app's MachineFonts): each
 * machine's own character ROM (vt_data.c's, SyncTERM's set) read in its
 * own set, so a cell's code point is drawn with the glyph its machine
 * drew for it; what a ROM hasn't got from Unscii's.  Made when first
 * asked for and kept.
 */
#include <stdlib.h>
#include <string.h>

#include "vtint.h"

/* the Atari ST's set from 0x80: Latin letters, Hebrew, Greek and
   mathematics */
static const uint32_t st_high[128] = {
	0xc7, 0xfc, 0xe9, 0xe2, 0xe4, 0xe0, 0xe5, 0xe7,
	0xea, 0xeb, 0xe8, 0xef, 0xee, 0xec, 0xc4, 0xc5,
	0xc9, 0xe6, 0xc6, 0xf4, 0xf6, 0xf2, 0xfb, 0xf9,
	0xff, 0xd6, 0xdc, 0xa2, 0xa3, 0xa5, 0xdf, 0x192,
	0xe1, 0xed, 0xf3, 0xfa, 0xf1, 0xd1, 0xaa, 0xba,
	0xbf, 0x2310, 0xac, 0xbd, 0xbc, 0xa1, 0xab, 0xbb,
	0xe3, 0xf5, 0xd8, 0xf8, 0x153, 0x152, 0xc0, 0xc3,
	0xd5, 0xa8, 0xb4, 0x2020, 0xb6, 0xa9, 0xae, 0x2122,
	0x133, 0x132, 0x5d0, 0x5d1, 0x5d2, 0x5d3, 0x5d4, 0x5d5,
	0x5d6, 0x5d7, 0x5d8, 0x5d9, 0x5db, 0x5dc, 0x5de, 0x5e0,
	0x5e1, 0x5e2, 0x5e4, 0x5e6, 0x5e7, 0x5e8, 0x5e9, 0x5ea,
	0x5df, 0x5da, 0x5dd, 0x5e3, 0x5e5, 0xa7, 0x2227, 0x221e,
	0x3b1, 0x3b2, 0x393, 0x3c0, 0x3a3, 0x3c3, 0xb5, 0x3c4,
	0x3a6, 0x398, 0x3a9, 0x3b4, 0x222e, 0x3d5, 0x2208, 0x2229,
	0x2261, 0xb1, 0x2265, 0x2264, 0x2320, 0x2321, 0xf7, 0x2248,
	0xb0, 0x2022, 0xb7, 0x221a, 0x207f, 0xb2, 0xb3, 0xaf,
};

uint32_t atari_st_char(int b)
{
	return b >= 0x80 && b <= 0xff ? st_high[b - 0x80] : (uint32_t)b;
}

/* teletext's English set, 0x20-0x7f: £ for #, arrows, fractions, # for
   _, a dash, ‖, ÷ and a block */
uint32_t viewdata_g0(int c)
{
	switch (c) {
	case 0x23: return 0xa3;
	case 0x5b: return 0x2190;
	case 0x5c: return 0xbd;
	case 0x5d: return 0x2192;
	case 0x5e: return 0x2191;
	case 0x5f: return '#';
	case 0x60: return 0x2014;
	case 0x7b: return 0xbc;
	case 0x7c: return 0x2016;
	case 0x7d: return 0xbe;
	case 0x7e: return 0xf7;
	case 0x7f: return 0x25a0;
	}
	return (uint32_t)c;
}

/* a font being made: its glyphs by code point, the first one put kept */
struct builder {
	int h, n, cap;
	uint32_t *codes;
	uint8_t *rows;
};

static int b_has(const struct builder *b, uint32_t cp)
{
	int i;
	for (i = 0; i < b->n; i++)
		if (b->codes[i] == cp)
			return 1;
	return 0;
}

static void b_put(struct builder *b, uint32_t cp, const uint8_t *rows)
{
	if (cp == 0 || b_has(b, cp))
		return;
	if (b->n == b->cap) {
		int cap = b->cap ? b->cap * 2 : 512;
		uint32_t *c = (uint32_t *)realloc(b->codes, cap * sizeof(*c));
		uint8_t *r = (uint8_t *)realloc(b->rows, (size_t)cap * b->h);
		if (!c || !r) {
			free(c ? c : b->codes);
			free(r ? r : b->rows);
			b->codes = NULL;
			b->rows = NULL;
			b->n = b->cap = 0;
			return;
		}
		b->codes = c;
		b->rows = r;
		b->cap = cap;
	}
	b->codes[b->n] = cp;
	memcpy(b->rows + (size_t)b->n * b->h, rows, b->h);
	b->n++;
}

/* code's glyph of ROM r, h (8 or 16) rows: its own, or its 8 doubled or
   its 16 halved (both rows' pixels, so a thin stroke stays) */
static void rom_glyph(int r, int code, int h, uint8_t *out)
{
	const struct vt_rom *rom = &vt_roms[r];
	int i;
	if (h == 16 && rom->r16) {
		memcpy(out, rom->r16 + code * 16, 16);
	} else if (h == 8 && rom->r8) {
		memcpy(out, rom->r8 + code * 8, 8);
	} else if (h == 16) {
		for (i = 0; i < 16; i++)
			out[i] = rom->r8[code * 8 + i / 2];
	} else {
		for (i = 0; i < 8; i++)
			out[i] = rom->r16[code * 16 + 2 * i] |
				 rom->r16[code * 16 + 2 * i + 1];
	}
}

/* ROM r's codes from..to, each as the code point map gives it (0: none) */
static void b_rom(struct builder *b, int r, int from, int to,
		  uint32_t (*map)(int, const void *), const void *ctx)
{
	uint8_t g[16];
	int c;
	for (c = from; c <= to; c++) {
		uint32_t cp = map(c, ctx);
		if (cp == 0 || b_has(b, cp))
			continue;
		rom_glyph(r, c, b->h, g);
		b_put(b, cp, g);
	}
}

static void b_font(struct builder *b, const struct vt_font *f)
{
	int i;
	if (!f || f->h != b->h)
		return;
	for (i = 0; i < f->n; i++)
		b_put(b, f->codes[i], f->rows8 + (size_t)i * f->h);
}

static int cmp_code(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	return x < y ? -1 : x > y;
}

/* the font made: its glyphs sorted by code point */
static const struct vt_font *b_done(struct builder *b)
{
	struct vt_font *f = (struct vt_font *)calloc(1, sizeof(*f));
	uint32_t *idx, *codes;
	uint8_t *rows;
	int i;
	/* (each code and its place, sorted together) */
	idx = (uint32_t *)malloc(sizeof(uint32_t) * 2 * (b->n ? b->n : 1));
	codes = (uint32_t *)malloc(sizeof(uint32_t) * (b->n ? b->n : 1));
	rows = (uint8_t *)malloc((size_t)(b->n ? b->n : 1) * b->h);
	if (!f || !idx || !codes || !rows) {
		free(f);
		free(idx);
		free(codes);
		free(rows);
		free(b->codes);
		free(b->rows);
		return NULL;
	}
	for (i = 0; i < b->n; i++) {
		idx[2 * i] = b->codes[i];
		idx[2 * i + 1] = (uint32_t)i;
	}
	qsort(idx, b->n, 2 * sizeof(uint32_t), cmp_code);
	for (i = 0; i < b->n; i++) {
		codes[i] = idx[2 * i];
		memcpy(rows + (size_t)i * b->h,
		       b->rows + (size_t)idx[2 * i + 1] * b->h, b->h);
	}
	free(idx);
	free(b->codes);
	free(b->rows);
	f->w = 8;
	f->h = b->h;
	f->n = b->n;
	f->codes = codes;
	f->rows8 = rows;
	f->rows32 = NULL;
	return f;
}

/* octet b's code point in a charset (0 for a control) */
static uint32_t octet(int b, const void *ctx)
{
	int cs = *(const int *)ctx;
	if (b < 0x20 || b == 0x7f)
		return 0;
	if (cs == CS_CP437)
		return cp437_decode(b);
	if (b >= 0x80 && b <= 0x9f)
		return 0;
	if (cs == CS_LATIN1)
		return (uint32_t)b;
	return b < 0x80 ? (uint32_t)b : 0;
}

static uint32_t in_set(int b, const void *ctx)
{
	return ((const uint32_t *)ctx)[b];
}

static uint32_t st_map(int b, const void *ctx)
{
	(void)ctx;
	return atari_st_char(b);
}

static uint32_t g0_map(int b, const void *ctx)
{
	(void)ctx;
	return viewdata_g0(b);
}

static const struct vt_font *fallback(int h)
{
	return h == 16 ? &vt_font_vga : &vt_font_8x8;
}

/* ROM r read in charset cs, h rows */
static const struct vt_font *in_charset(int r, int h, int cs)
{
	struct builder b;
	memset(&b, 0, sizeof(b));
	b.h = h;
	b_rom(&b, r, 0x20, 0xff, octet, &cs);
	b_font(&b, fallback(h));
	return b_done(&b);
}

static const struct vt_font *make(int which)
{
	struct builder b;
	memset(&b, 0, sizeof(b));
	b.h = 8;
	switch (which) {
	case MF_VGA:
		return in_charset(ROM_VGA, 16, CS_CP437);
	case MF_RIPTERM:
		return in_charset(ROM_RIPTERM, 8, CS_CP437);
	case MF_TOPAZ:
		return in_charset(ROM_TOPAZ, 8, CS_LATIN1);
	case MF_C64:
	case MF_C128:
		b_rom(&b, which == MF_C64 ? ROM_C64_UPPER : ROM_C128_UPPER,
		      0, 0xff, in_set, vt_set_c64_upper);
		b_rom(&b, which == MF_C64 ? ROM_C64_LOWER : ROM_C128_LOWER,
		      0, 0xff, in_set, vt_set_c64_lower);
		break;
	case MF_ATARI:
		b_rom(&b, ROM_ATARI, 0, 0x7f, in_set, vt_set_atari_graphics);
		break;
	case MF_ATARI_ST:
		b_rom(&b, ROM_ATARI_ST, 0x20, 0xff, st_map, NULL);
		break;
	case MF_PRESTEL:
		b.h = 16;
		b_rom(&b, ROM_PRESTEL, 0x20, 0x7f, g0_map, NULL);
		break;
	default:
		return NULL;
	}
	b_font(&b, fallback(b.h));
	return b_done(&b);
}

static const struct vt_font *made[MF_COUNT];

const struct vt_font *machine_font(int which)
{
	if (which < 0 || which >= MF_COUNT)
		return NULL;
	if (!made[which])
		made[which] = make(which);
	return made[which];
}

/* SyncTERM's numbers (CSI SP D's) and the ROMs */
static const int syncterm_rom[][2] = {
	{ 0, ROM_VGA }, { 26, ROM_VGA_THIN }, { 32, ROM_C64_UPPER },
	{ 33, ROM_C64_LOWER }, { 34, ROM_C128_UPPER }, { 35, ROM_C128_LOWER },
	{ 36, ROM_ATARI }, { 37, ROM_POT_NOODLE }, { 38, ROM_MOSOUL },
	{ 39, ROM_MICROKNIGHT_PLUS }, { 40, ROM_TOPAZ_PLUS },
	{ 41, ROM_MICROKNIGHT }, { 42, ROM_TOPAZ }, { 43, ROM_PRESTEL },
	{ 44, ROM_ATARI_ST }
};
#define N_SYNCTERM ((int)(sizeof(syncterm_rom) / sizeof(syncterm_rom[0])))

/* each one made, by ROM, height (8 or 16) and charset */
static const struct vt_font *synctermed[ROM_COUNT][2][3];

const struct vt_font *syncterm_font(int n, int height, int charset)
{
	int i, r = -1, hi = height == 16 ? 1 : 0;
	for (i = 0; i < N_SYNCTERM; i++)
		if (syncterm_rom[i][0] == n)
			r = syncterm_rom[i][1];
	if (r < 0 || charset < 0 || charset > 2)
		return NULL;
	if (!synctermed[r][hi][charset])
		synctermed[r][hi][charset] = in_charset(r, hi ? 16 : 8, charset);
	return synctermed[r][hi][charset];
}
