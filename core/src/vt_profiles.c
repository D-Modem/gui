/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * vt_profiles.c - the kinds of terminal there are (the app's
 * TerminalProfiles): what each calls itself to SSH, Telnet and the
 * programs that ask, its answers, its machine's screen, palette and
 * font, and the interpreter that reads it.
 */
#include <string.h>

#include "vtint.h"

/* ------------------------------------------------------------------ */
/* the palettes (opaque ARGB) */

/* an Amiga's ANSI colours, from its 4-bit levels */
static const uint32_t amiga_colors[16] = {
	0xff000000, 0xffcc2222, 0xff22cc22, 0xffcccc22,
	0xff2222cc, 0xffcc22cc, 0xff22cccc, 0xffcccccc,
	0xff666666, 0xffff5555, 0xff55ff55, 0xffffff55,
	0xff5555ff, 0xffff55ff, 0xff55ffff, 0xffffffff
};

/* Philip Timmermann's "Pepto" measurements of the VIC-II */
static const uint32_t pepto[16] = {
	0xff000000, 0xffffffff, 0xff68372b, 0xff70a4b2,
	0xff6f3d86, 0xff588d43, 0xff352879, 0xffb8c76f,
	0xff6f4f25, 0xff433900, 0xff9a6759, 0xff444444,
	0xff6c6c6c, 0xff9ad284, 0xff6c5eb5, 0xff959595
};

/* the VIC-20's: eight, then the lighter ones */
static const uint32_t vic20[16] = {
	0xff000000, 0xffffffff, 0xffb61f21, 0xff4df0ff,
	0xffb43fff, 0xff44e237, 0xff1a34ff, 0xffdcd71b,
	0xffca5400, 0xffe9b072, 0xffe79293, 0xff9af7fd,
	0xffe09fff, 0xff8fe493, 0xff8290ff, 0xffe5de85
};

/* the C128's VDC (RGBI), each VIC-II colour as the 80-column screen
   shows it */
static const uint32_t vdc[16] = {
	0xff000000, 0xffffffff, 0xffaa0000, 0xff55ffff,
	0xffff55ff, 0xff00aa00, 0xff0000aa, 0xffffff55,
	0xffaa00aa, 0xffaa5500, 0xffff5555, 0xff555555,
	0xff00aaaa, 0xff55ff55, 0xff5555ff, 0xffaaaaaa
};

/* the Plus/4's TED */
static const uint32_t ted[16] = {
	0xff000000, 0xffffffff, 0xff9c3e3e, 0xff5ab5b5,
	0xff9f4ba8, 0xff5aa33e, 0xff5250c4, 0xffb5b54a,
	0xffa36a2b, 0xff7d5a1b, 0xff85a33e, 0xffc7637a,
	0xff3ea383, 0xff6a8cd9, 0xff4f5fb3, 0xff7ec26b
};

/* a PET's phosphor */
static const uint32_t pet_green[2] = { 0xff000000, 0xff41ff00 };

/* an Atari 8-bit's blue and light text */
static const uint32_t atari_blue[2] = { 0xff1d5dae, 0xffa8d8ff };

/* TOS's sixteen at power-up (3-bit levels) */
static const uint32_t tos[16] = {
	0xffffffff, 0xffff0000, 0xff00ff00, 0xffffff00,
	0xff0000ff, 0xffff00ff, 0xff00ffff, 0xffb6b6b6,
	0xff6d6d6d, 0xffff6d6d, 0xff6dff6d, 0xffffff6d,
	0xff6d6dff, 0xffff6dff, 0xff6dffff, 0xff000000
};
/* the ST's high resolution colour mode: TOS's first three and black */
static const uint32_t tos_med[4] = {
	0xffffffff, 0xffff0000, 0xff00ff00, 0xff000000
};

/* teletext's eight */
static const uint32_t teletext[8] = {
	0xff000000, 0xffff0000, 0xff00ff00, 0xffffff00,
	0xff0000ff, 0xffff00ff, 0xff00ffff, 0xffffffff
};

/* a Commodore's CTRL 1-8 and C= 1-8 */
static const uint8_t cbm_ctrl[8] = {
	0x90, 0x05, 0x1c, 0x9f, 0x9c, 0x1e, 0x1f, 0x9e
};
static const uint8_t cbm_cbm[8] = {
	0x81, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b
};

/* ------------------------------------------------------------------ */
/* the profiles (in the struct's order: no designated initialisers for
   Open Watcom) */

/* a DEC terminal */
#define DEC(id, label, term, telnet, level, features, da1, da2, home)     \
	{ id, label, term, telnet, K_DEC, level, features, da1, da2, -1,  \
	  home, MF_NONE, 0, NULL, 0, 0, 0, 0, 0, NULL, NULL, NULL, NULL,  \
	  0, 0, 0, 0, 0, NULL, NULL }

#define XTERM_FEATURES (F_PRINTER | F_COLOR | F_SIXEL | F_XTERM | F_TEK)
#define XTERM_DA1 "\x1b[?62;4;22c"
#define XTERM_DA2 "\x1b[>41;390;0c"

/* a BBS terminal: xterm with CTerm's additions */
#define BBS(id, label, term, kind, charset, font, syncterm, colors, n)    \
	{ id, label, term, "ANSI", kind, 2, XTERM_FEATURES, XTERM_DA1,    \
	  XTERM_DA2, charset, 1, font, syncterm, colors, n, 0, 0, 0, 0,   \
	  NULL, NULL, NULL, NULL, 0, 0, 0, 0, 0, NULL, NULL }

/* a Commodore */
#define CBM(id, label, font, pal, cols, rows, fg, up, lo, nctrl, ncbm,    \
	    esc, attr, tablf)                                             \
	{ id, label, "petscii", "PETSCII", K_PETSCII, 0, 0, NULL, NULL,   \
	  -1, 0, font, 0, pal, (int)(sizeof(pal) / sizeof(pal[0])), cols, \
	  rows, fg, 0, up, lo, cbm_ctrl, cbm_cbm, nctrl, ncbm, esc, attr, \
	  tablf, NULL, ".seq" }

/* another machine */
#define MACH(id, label, term, telnet, kind, font, pal, cols, rows, fg,    \
	     atascii, ext)                                                \
	{ id, label, term, telnet, kind, 0, 0, NULL, NULL, -1, 0, font,   \
	  0, pal, (int)(sizeof(pal) / sizeof(pal[0])), cols, rows, fg, 0, \
	  NULL, NULL, NULL, NULL, 0, 0, 0, 0, 0, atascii, ext }

static const struct vt_profile xterm = DEC("xterm", "xterm",
	"xterm-256color", "XTERM-256COLOR", 2, XTERM_FEATURES, XTERM_DA1,
	XTERM_DA2, 1);
static const struct vt_profile ansi_bbs = BBS("ansi-bbs", "ANSI-BBS (PC)",
	"ansi", K_ANSI_BBS, CS_CP437, MF_VGA, 0, NULL, 0);
static const struct vt_profile rip = BBS("rip", "RIPscrip", "rip", K_RIP,
	CS_CP437, MF_RIPTERM, 45, NULL, 0);
static const struct vt_profile avatar = BBS("avatar", "Avatar (AVT/0+)",
	"avatar", K_AVATAR, CS_CP437, MF_VGA, 0, NULL, 0);
static const struct vt_profile amiga = BBS("amiga", "Amiga ANSI", "amiga",
	K_ANSI_BBS, CS_LATIN1, MF_TOPAZ, 42, amiga_colors, 16);

static const struct vt_profile vt52 = {
	"vt52", "DEC VT52", "vt52", "DEC-VT52", K_VT52, 0, 0, NULL, NULL, -1,
	0, MF_NONE, 0, NULL, 0, 0, 0, 0, 0, NULL, NULL, NULL, NULL, 0, 0, 0,
	0, 0, NULL, NULL
};
const struct vt_profile vt_profile_vt100 = DEC("vt100", "DEC VT100",
	"vt100", "DEC-VT100", 1, 0, "\x1b[?1;2c", NULL, 0);
static const struct vt_profile vt102 = DEC("vt102", "DEC VT102", "vt102",
	"DEC-VT102", 1, F_PRINTER, "\x1b[?6c", NULL, 0);
static const struct vt_profile vt220 = DEC("vt220", "DEC VT220", "vt220",
	"DEC-VT220", 2, F_PRINTER, "\x1b[?62;1;2;6;7;8;9c", "\x1b[>1;10;0c", 0);
static const struct vt_profile vt320 = DEC("vt320", "DEC VT320", "vt320",
	"DEC-VT320", 3, F_PRINTER, "\x1b[?63;1;2;6;7;8;9c", "\x1b[>24;10;0c",
	0);
static const struct vt_profile vt340 = DEC("vt340", "DEC VT340", "vt340",
	"DEC-VT340", 3, F_PRINTER | F_SIXEL | F_REGIS | F_TEK,
	"\x1b[?63;1;2;3;4;6;7;8;9c", "\x1b[>19;10;0c", 0);
static const struct vt_profile vt420 = DEC("vt420", "DEC VT420", "vt420",
	"DEC-VT420", 4, F_PRINTER, "\x1b[?64;1;2;6;7;8;9;15;18;21;28c",
	"\x1b[>41;10;0c", 0);
static const struct vt_profile vt510 = DEC("vt510", "DEC VT510", "vt510",
	"DEC-VT510", 5, F_PRINTER, "\x1b[?65;1;2;6;7;8;9;15;18;21;28c",
	"\x1b[>61;10;0c", 0);
static const struct vt_profile vt520 = DEC("vt520", "DEC VT520", "vt520",
	"DEC-VT520", 5, F_PRINTER, "\x1b[?65;1;2;6;7;8;9;15;18;21;28c",
	"\x1b[>64;10;0c", 0);
static const struct vt_profile vt525 = DEC("vt525", "DEC VT525", "vt525",
	"DEC-VT525", 5, F_PRINTER | F_COLOR,
	"\x1b[?65;1;2;6;7;8;9;15;18;21;22;28c", "\x1b[>65;10;0c", 0);

static const struct vt_profile c64 = CBM("c64", "Commodore 64", MF_C64,
	pepto, 40, 25, 14, vt_set_c64_upper, vt_set_c64_lower, 8, 8, 0,
	AC_NONE, 0);
static const struct vt_profile c128 = CBM("c128",
	"Commodore 128 (40 col.)", MF_C128, pepto, 40, 25, 14,
	vt_set_c64_upper, vt_set_c64_lower, 8, 8, 1, AC_NONE, 1);
static const struct vt_profile c128_80 = CBM("c128-80",
	"Commodore 128 (80 col.)", MF_C128, vdc, 80, 25, 14,
	vt_set_c64_upper, vt_set_c64_lower, 8, 8, 1, AC_VDC, 1);
static const struct vt_profile plus4 = CBM("plus4", "Commodore Plus/4",
	MF_C64, ted, 40, 25, 14, vt_set_c64_upper, vt_set_c64_lower, 8, 8, 1,
	AC_PLUS4, 0);
static const struct vt_profile vic20p = CBM("vic20", "Commodore VIC-20",
	MF_C64, vic20, 22, 23, 1, vt_set_vic_upper, vt_set_vic_lower, 8, 0, 0,
	AC_NONE, 0);
static const struct vt_profile pet = CBM("pet", "Commodore PET (40 col.)",
	MF_C64, pet_green, 40, 25, 1, vt_set_pet_upper, vt_set_pet_lower, 0, 0,
	0, AC_NONE, 0);
static const struct vt_profile pet80 = CBM("pet80",
	"Commodore PET (80 col.)", MF_C64, pet_green, 80, 25, 1,
	vt_set_pet_upper, vt_set_pet_lower, 0, 0, 0, AC_NONE, 0);

static const struct vt_profile atari = MACH("atari", "Atari 8-bit",
	"atascii", "ATASCII", K_ATASCII, MF_ATARI, atari_blue, 40, 24, 1,
	vt_set_atari_graphics, NULL);
static const struct vt_profile atari_intl = MACH("atari-intl",
	"Atari 8-bit (international)", "atascii", "ATASCII", K_ATASCII,
	MF_ATARI, atari_blue, 40, 24, 1, vt_set_atari_international, NULL);
static const struct vt_profile atari_st = MACH("atari-st",
	"Atari ST (80 col.)", "st52", "VT52", K_ATARI_ST, MF_ATARI_ST,
	tos_med, 80, 25, 3, NULL, NULL);
static const struct vt_profile atari_st_low = MACH("atari-st-low",
	"Atari ST (40 col., 16 colours)", "st52", "VT52", K_ATARI_ST,
	MF_ATARI_ST, tos, 40, 25, 15, NULL, NULL);
static const struct vt_profile viewdata = MACH("viewdata",
	"Viewdata (Prestel)", "viewdata", "VIEWDATA", K_VIEWDATA, MF_PRESTEL,
	teletext, 40, 24, 7, NULL, ".vdt");
static const struct vt_profile minitel = MACH("minitel", "Minitel",
	"minitel", "MINITEL", K_MINITEL, MF_NONE, teletext, 40, 25, 7, NULL,
	".vdt");

static const struct vt_profile tek4014 = {
	"tek4014", "Tektronix 4014", "tek4014", "TEK4014", K_TEK, 0, 0, NULL,
	NULL, -1, 0, MF_NONE, 0, NULL, 0, 0, 0, 0, 0, NULL, NULL, NULL, NULL,
	0, 0, 0, 0, 0, NULL, NULL
};

/* in the pickers' order */
const struct vt_profile *const vt_profiles[] = {
	&xterm, &ansi_bbs, &rip, &avatar, &amiga, &vt52, &vt_profile_vt100,
	&vt102, &vt220, &vt320, &vt340, &vt420, &vt510, &vt520, &vt525, &c64,
	&c128, &c128_80, &plus4, &vic20p, &pet, &pet80, &atari, &atari_intl,
	&atari_st, &atari_st_low, &viewdata, &minitel, &tek4014
};
const int vt_profile_count =
	(int)(sizeof(vt_profiles) / sizeof(vt_profiles[0]));

const struct vt_profile *vt_profile_by_id(const char *id)
{
	int i;
	if (!id)
		return NULL;
	for (i = 0; i < vt_profile_count; i++)
		if (!strcmp(vt_profiles[i]->id, id))
			return vt_profiles[i];
	return NULL;
}

/* the default: what the terminal was before profiles */
const struct vt_profile *vt_profile_default(void)
{
	return &xterm;
}

struct vt_interp *profile_interp(dmc_vt *t, const struct vt_profile *p)
{
	switch (p->kind) {
	case K_VT52:
		return vt52_new(t, G_VT52_GRAPHICS, "\x1b/K", NULL, NULL);
	case K_ATARI_ST: return atari_st_new(t, p);
	case K_PETSCII: return petscii_new(t, p);
	case K_ATASCII: return atascii_new(t, p);
	case K_VIEWDATA: return viewdata_new(t, p);
	case K_MINITEL: return minitel_new(t, p);
	case K_TEK: return tek_new(t, NULL, NULL);
	default: return dec_new(t, p);
	}
}
