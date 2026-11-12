/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * vtint.h - the terminal emulator's insides (dmc_vt), shared by its
 *           files: the screen (vt.c), the interpreters of each family of
 *           terminals (vt_dec.c, vt_vt52.c, vt_machine.c, vt_gfx.c,
 *           vt_regis.c, vt_rip.c), the character sets and keys
 *           (vt_charsets.c, vt_keys.c), the profiles (vt_profiles.c) and
 *           the machines' fonts (vt_data.c).
 *
 * A C port of the D-Modem app's shared Kotlin emulator
 * (D-Modem/app: shared/src/commonMain/kotlin/org/dmodem/app/terminal),
 * kept close to it so that a change there can be carried here: the same
 * parts, the same names where C allows, the same behaviour.  C99 as
 * Open Watcom takes it (no designated initialisers, no VLAs).
 */
#ifndef DMC_VTINT_H
#define DMC_VTINT_H

#include <stddef.h>
#include <stdint.h>

#include "dmodemctl.h"
#include "util.h"

/*
 * A cell's look, packed in 64 bits as the app's Style is: the foreground
 * (bits 0-24), the background (25-49) and the attributes (50-63).  A
 * colour is an index of the 256-colour palette, ST_DEFAULT_FG or
 * ST_DEFAULT_BG, or ST_RGB or'ed with a 24-bit colour.
 */
typedef uint64_t vt_style;

#define ST_DEFAULT_FG 256
#define ST_DEFAULT_BG 257
#define ST_RGB 0x1000000

enum {
	A_BOLD = 1, A_FAINT = 2, A_ITALIC = 4, A_UNDERLINE = 8, A_BLINK = 16,
	A_INVERSE = 32, A_INVISIBLE = 64, A_STRIKE = 128,
	A_DUNDER = 256,         /* SGR 21, 4:2 */
	A_CURLY = 512,          /* SGR 4:3 (and 4:4, 4:5) */
	A_OVERLINE = 1024,
	A_PROTECTED = 2048      /* DECSCA's (not a rendition) */
};
#define A_ANY_UNDER (A_UNDERLINE | A_DUNDER | A_CURLY)

#define ST_COLOR_MASK 0x1ffffffULL

#define st_of(fg, bg, attrs) \
	(((vt_style)(unsigned)(fg) & ST_COLOR_MASK) | \
	 (((vt_style)(unsigned)(bg) & ST_COLOR_MASK) << 25) | \
	 ((vt_style)(unsigned)(attrs) << 50))
#define st_fg(s) ((int)((s) & ST_COLOR_MASK))
#define st_bg(s) ((int)(((s) >> 25) & ST_COLOR_MASK))
#define st_attrs(s) ((int)((s) >> 50))
#define st_with_fg(s, f) st_of((f), st_bg(s), st_attrs(s))
#define st_with_bg(s, b) st_of(st_fg(s), (b), st_attrs(s))
#define st_with_attrs(s, a) st_of(st_fg(s), st_bg(s), (a))
/* the look of erased cells: only the background stays (xterm's BCE) */
#define st_erased(s) st_of(ST_DEFAULT_FG, st_bg(s), 0)
#define ST_NORMAL st_of(ST_DEFAULT_FG, ST_DEFAULT_BG, 0)

/* the right half of a wide character, in a line's chars */
#define WIDE_TAIL (-1)

/* a picture (a sixel image, a graphics terminal's screen), its pixels
   ARGB (0: transparent), shared by the cells that show it */
struct vt_image {
	int refs;
	uint32_t id;
	int width, height;
	uint32_t *pixels;
	/* the cell size it was placed at: its tiles are cells of that size */
	int cell_w, cell_h;
	/* a graphics terminal's picture over the screen, not an image in
	   the text */
	int picture;
};

/* a hyperlink (OSC 8), shared by the cells that carry it */
struct vt_link {
	int refs;
	char *uri;
};

/* what few cells carry besides their character */
struct vt_extra {
	uint32_t *comb;         /* combining marks */
	int ncomb;
	struct vt_image *img;   /* a picture's tile */
	int tx, ty;
	struct vt_link *link;
};

enum { LS_SINGLE, LS_DOUBLE_WIDTH, LS_DOUBLE_TOP, LS_DOUBLE_BOTTOM };

/* a line of cells: code points (0 never written, WIDE_TAIL), their
   looks, and the few cells' extras */
struct vt_line {
	int cols;
	int32_t *ch;
	vt_style *st;
	struct vt_extra **ex;   /* NULL until a cell needs one */
	int wrapped;
	int size;               /* LS_* */
};

struct vt_line *line_new(int cols, vt_style st);
void line_free(struct vt_line *l);
struct vt_extra *line_extra(const struct vt_line *l, int i);
struct vt_extra *line_extra_for(struct vt_line *l, int i);
void line_set(struct vt_line *l, int i, int32_t cp, vt_style st);
void line_clear(struct vt_line *l, int from, int to, vt_style st);
void line_blank(struct vt_line *l, vt_style st);
void line_insert_cells(struct vt_line *l, int at, int n, vt_style st, int end);
void line_delete_cells(struct vt_line *l, int at, int n, vt_style st, int end);
void line_resize(struct vt_line *l, int n);
void line_copy_from(struct vt_line *l, const struct vt_line *o);
/* cell i's extras made those of e (copies; the picture and link shared) */
void extra_assign(struct vt_line *l, int i, const struct vt_extra *e);
/* the text from..to, trailing blanks dropped, as UTF-8 into b */
void line_text(const struct vt_line *l, int from, int to, dmc_buf *b);

struct vt_image *image_new(int w, int h, int cell_w, int cell_h, int picture);
void image_unref(struct vt_image *img);
struct vt_link *link_new(const char *uri);
void link_unref(struct vt_link *l);

void utf8_put(dmc_buf *b, uint32_t c);

/*
 * Bitmap glyphs (BitmapFont): w pixels wide (8, or up to 24 for a soft
 * font), h high; a row's leftmost pixel bit w - 1.  The codes are sorted.
 */
struct vt_font {
	int w, h, n;
	const uint32_t *codes;
	const uint8_t *rows8;   /* n * h, when w <= 8 */
	const uint32_t *rows32; /* n * h, otherwise */
};
int font_index(const struct vt_font *f, uint32_t cp);
uint32_t font_row(const struct vt_font *f, uint32_t cp, int row);

/* the machines' fonts and sets (vt_data.c, generated) */
extern const struct vt_font vt_font_8x8;        /* Unscii's 8x8 */
extern const struct vt_font vt_font_vga;        /* code page 437, 8x16 */
extern const uint32_t vt_set_c64_upper[256], vt_set_c64_lower[256];
extern const uint32_t vt_set_vic_upper[256], vt_set_vic_lower[256];
extern const uint32_t vt_set_pet_upper[256], vt_set_pet_lower[256];
extern const uint32_t vt_set_atari_graphics[256];
extern const uint32_t vt_set_atari_international[256];

/* the machines' character ROMs (SyncTERM's), 256 glyphs 8 wide in the
   machine's own order, 8 rows, 16 or both (NULL: not that size) */
enum {
	ROM_VGA, ROM_VGA_THIN, ROM_C64_UPPER, ROM_C64_LOWER, ROM_C128_UPPER,
	ROM_C128_LOWER, ROM_ATARI, ROM_POT_NOODLE, ROM_MOSOUL,
	ROM_MICROKNIGHT_PLUS, ROM_TOPAZ_PLUS, ROM_MICROKNIGHT, ROM_TOPAZ,
	ROM_PRESTEL, ROM_ATARI_ST, ROM_RIPTERM, ROM_COUNT
};
struct vt_rom {
	const uint8_t *r8, *r16;
};
extern const struct vt_rom vt_roms[ROM_COUNT];

/* the terminal types' fonts (vt_fonts.c): each ROM read in its machine's
   set, Unscii's for what it hasn't got */
enum {
	MF_NONE = -1, MF_VGA, MF_RIPTERM, MF_C64, MF_C128, MF_ATARI, MF_TOPAZ,
	MF_ATARI_ST, MF_PRESTEL, MF_COUNT
};
const struct vt_font *machine_font(int which);
/* SyncTERM's font n (CSI SP D's), height 8 or 16 rows, its codes read as
   octets of a charset (CS_*); NULL if it isn't here */
const struct vt_font *syncterm_font(int n, int height, int charset);
/* an octet in the Atari ST's set as its code point */
uint32_t atari_st_char(int b);
/* teletext's English set, 0x20-0x7f */
uint32_t viewdata_g0(int c);

/* the mosaics (teletext's 2x3 blocks): their code points and glyphs */
#define MOSAIC_BASE 0xf1000
uint32_t mosaic_code(int mask, int separated);
int mosaic_bits(int c);
extern const struct vt_font vt_font_mosaic;

/*
 * Character sets (vt_charsets.c)
 */
enum { CS_UTF8, CS_CP437, CS_LATIN1 };
uint32_t cp437_decode(int b);
int cp437_encode(uint32_t cp);
/* text into octets of a set (what it can't have: '?') */
void charset_encode(int cs, const uint32_t *cps, int n, dmc_buf *out);

/* the G sets a slot can hold */
enum {
	G_ASCII, G_DEC_GRAPHICS, G_UK, G_VT52_GRAPHICS, G_DEC_SUPPLEMENTAL,
	G_DEC_TECHNICAL, G_LATIN1, G_DUTCH, G_FINNISH, G_FRENCH,
	G_FRENCH_CANADIAN, G_GERMAN, G_ITALIAN, G_NORWEGIAN_DANISH,
	G_SPANISH, G_SWEDISH, G_SWISS, G_PORTUGUESE, G_SOFT, G_COUNT
};
int gset_by_designator(const char *d, int is96);
int gset_is96(int g);
int gset_level(int g);
/* its first designator ("" for none) */
const char *gset_designator(int g);
uint32_t gset_map(int g, uint32_t cp);
uint32_t dec_graphics(uint32_t cp);

/* the cells a character takes: 0, 1 or 2 */
int wcwidth_of(uint32_t cp);

/*
 * The emulator
 */
struct vt_interp;
struct vt_profile;

/* G0-G3 and the shifts, as DECSC keeps them */
struct vt_sets_saved {
	uint8_t g[4];
	int gl, gr;
};

struct vt_saved {
	int set;
	int x, y;
	vt_style style;
	int wrap, origin;
	int have_sets;
	struct vt_sets_saved sets;
};

/* a VT320's host-writable status line */
struct vt_status {
	struct vt_line *line;
	int x;
};

enum { MOUSE_NONE, MOUSE_X10, MOUSE_NORMAL, MOUSE_BUTTON, MOUSE_ANY };

#define VT_PAGES 6
#define VT_MAX_IMAGE 2048
#define VT_NO_COLOR 0

struct dmc_vt {
	int cols, rows;
	/* the scrollback: a ring, hist_n lines from hist_first */
	struct vt_line **hist;
	int hist_n, hist_max, hist_first;
	struct vt_line **main_s, **alt_s, **screen;
	/* the main screen's pages not shown (VT420), by number */
	struct vt_line **pages[VT_PAGES];
	int page;

	int cx, cy, pending_wrap;
	vt_style style;

	int cursor_visible, app_cursor, app_keypad, bracketed_paste;
	int reverse_video, mouse_mode, mouse_sgr, mouse_urxvt, mouse_utf8;
	int focus_events, cursor_style;
	int sixel_scrolling, sixel_cursor_right;
	int cell_pw, cell_ph;
	/* the remote's colours and the machines' palettes: 0-255, 256 fg,
	   257 bg, 258 cursor; VT_NO_COLOR or opaque ARGB */
	uint32_t color_over[259];
	int requested_cols, requested_rows;
	int ice_colors, emulated_bps, leds, vt52_keys;
	struct vt_link *link;
	int join_next;
	int autowrap, origin_mode, insert_mode, newline_mode;
	int top, bottom;
	uint8_t *tabs;
	int lr_margins, left, right;
	int32_t last_char;
	struct vt_saved saved_main, saved_alt;
	int home_on_clear;
	int eight_bit_replies;
	int status_type;
	struct vt_status status;
	const struct vt_font *soft_glyphs;
	/* the font the host chose (CSI SP D), NULL: the profile's */
	const struct vt_font *host_font;

	const struct vt_profile *profile;
	struct vt_interp *interp;

	/* the decoder */
	int charset;            /* in use */
	int user_charset;       /* the user's, for profiles without their own */
	int u_need;
	uint32_t u_cp, u_min;

	/* what the app reads */
	dmc_buf reply, text, title;
	int title_changed;
	int bells;
	uint32_t *music;        /* hz * 100, ms, rest ms */
	int music_n, music_cap;
	dmc_buf print;
	char answerback[32];
	int changed;
	uint32_t image_ids;

	/* the keys */
	int enter_mode;         /* 0 CR, 1 CR LF, 2 LF */
	int backspace_code;     /* 0x7f or 0x08 */
	/* the theme */
	uint32_t theme_fg, theme_bg, theme_cursor;
	uint32_t theme_pal[16];
	int bold_bright;
};

/* the glyphs the cells are drawn with: the host's font, else the
   profile's (NULL: the view's font) */
const struct vt_font *vt_glyphs(const dmc_vt *t);

/* the screen's operations (vt.c), as TerminalEmulator's */
struct vt_line *vt_line(const dmc_vt *t, int row);
void vt_print(dmc_vt *t, int32_t cp);
int vt_line_width(const dmc_vt *t, int y);
void vt_set_line_size(dmc_vt *t, int size);
void vt_set_column_mode(dmc_vt *t, int wide);
void vt_carriage_return(dmc_vt *t);
void vt_backspace(dmc_vt *t);
void vt_index(dmc_vt *t);
void vt_reverse_index(dmc_vt *t);
void vt_scroll_up(dmc_vt *t, int top, int bottom);
void vt_scroll_down(dmc_vt *t, int top, int bottom);
void vt_move_cursor(dmc_vt *t, int x, int y, int clamp_to_region);
void vt_cursor_position(dmc_vt *t, int x, int y);
void vt_set_margins(dmc_vt *t, int l, int r);
void vt_set_lr_margins(dmc_vt *t, int on);
void vt_set_region(dmc_vt *t, int top, int bottom);
void vt_tab(dmc_vt *t, int n);
void vt_back_tab(dmc_vt *t, int n);
void vt_set_tab(dmc_vt *t);
void vt_clear_tab(dmc_vt *t);
void vt_clear_tabs(dmc_vt *t);
void vt_erase_display(dmc_vt *t, int mode, int selective);
void vt_erase_line(dmc_vt *t, int mode, int selective);
void vt_erase_chars(dmc_vt *t, int n);
void vt_insert_chars(dmc_vt *t, int n);
void vt_delete_chars(dmc_vt *t, int n);
void vt_insert_lines(dmc_vt *t, int n);
void vt_delete_lines(dmc_vt *t, int n);
void vt_set_alt_screen(dmc_vt *t, int on, int clear);
int vt_alt_screen(const dmc_vt *t);
void vt_save_cursor(dmc_vt *t);
void vt_restore_cursor(dmc_vt *t);
void vt_alignment_test(dmc_vt *t);
void vt_soft_reset(dmc_vt *t);
void vt_full_reset(dmc_vt *t);
void vt_select_page(dmc_vt *t, int n, int home);
int vt_within_margins(const dmc_vt *t);
void vt_set_color_override(dmc_vt *t, int i, uint32_t argb);
void vt_reset_colors(dmc_vt *t, int from, int to);
uint32_t vt_color_override(const dmc_vt *t, int i);
/* the colour the screen shows for i (0-255, 256 fg, 257 bg, 258 cursor) */
uint32_t vt_shown_color(const dmc_vt *t, int i);
/* s to the remote as Latin-1 octets (C1 forms when eight_bit_replies) */
void vt_reply(dmc_vt *t, const char *s);
void vt_replyf(dmc_vt *t, const char *fmt, ...);
void vt_bell(dmc_vt *t);
void vt_add_music(dmc_vt *t, double hz, int ms, int rest_ms);
void vt_print_out(dmc_vt *t, const char *utf8);
void vt_set_title(dmc_vt *t, const char *s);
void vt_place_image(dmc_vt *t, struct vt_image *img);
void vt_show_picture(dmc_vt *t, struct vt_image *img);
void vt_tab_stops(const dmc_vt *t, dmc_buf *out);
/* a machine's size and palette on the screen, and back */
void vt_use_machine(dmc_vt *t, int cols, int rows, const uint32_t *pal,
		    int npal, int fg, int bg);
void vt_release_machine(dmc_vt *t);
/* the status line's text as the view shows it */
void vt_status_text(const dmc_vt *t, dmc_buf *out);
void status_print(dmc_vt *t, int32_t cp, vt_style st);
void status_move(dmc_vt *t, int col);
void status_erase(dmc_vt *t, int mode, vt_style st);

/*
 * The interpreters: what reads the remote's characters
 */
struct vt_interp_ops {
	void (*process)(struct vt_interp *, int32_t cp);
	void (*reset)(struct vt_interp *);
	void (*soft_reset)(struct vt_interp *);
	/* G0-G3 for DECSC (NULL: none) */
	int (*save_sets)(struct vt_interp *, struct vt_sets_saved *);
	void (*restore_sets)(struct vt_interp *, const struct vt_sets_saved *);
	/* a key's octets into out: 1 if it has its own (even none), 0 the
	   VT sequences */
	int (*key)(struct vt_interp *, int key, int mods, dmc_buf *out);
	int (*chr)(struct vt_interp *, uint32_t cp, int mods, dmc_buf *out);
	int (*tap)(struct vt_interp *, double fx, double fy, dmc_buf *out);
	void (*flush)(struct vt_interp *);
	void (*detach)(struct vt_interp *);
	void (*destroy)(struct vt_interp *);
};

struct vt_interp {
	const struct vt_interp_ops *ops;
	dmc_vt *t;
	int reads_octets;
};

void interp_free(struct vt_interp *in);

/* a DEC terminal's printer port (vt_dec.c): what the remote sends to the
   printer until ends[] (controller mode), lines as they're left (auto) */
struct vt_printer {
	dmc_vt *t;
	int controller, autop;
	dmc_buf buf;
	const char *ends[2];
};
void printer_init(struct vt_printer *p, dmc_vt *t, const char *end1,
		  const char *end2);
void printer_start(struct vt_printer *p);
void printer_feed(struct vt_printer *p, int32_t cp);
void printer_line(struct vt_printer *p, int y);
void printer_screen(struct vt_printer *p);
void printer_reset(struct vt_printer *p);
void printer_free(struct vt_printer *p);

/* the key modifiers */
enum { M_SHIFT = 1, M_ALT = 2, M_CTRL = 4 };

/* the interpreters there are */
struct vt_interp *dec_new(dmc_vt *t, const struct vt_profile *p);
struct vt_interp *vt52_new(dmc_vt *t, int graphics_set, const char *identity,
			   void (*on_ansi)(void *), void *ctx);
struct vt_interp *atari_st_new(dmc_vt *t, const struct vt_profile *p);
struct vt_interp *petscii_new(dmc_vt *t, const struct vt_profile *p);
struct vt_interp *atascii_new(dmc_vt *t, const struct vt_profile *p);
struct vt_interp *viewdata_new(dmc_vt *t, const struct vt_profile *p);
struct vt_interp *minitel_new(dmc_vt *t, const struct vt_profile *p);
struct vt_interp *tek_new(dmc_vt *t, void (*on_exit)(void *), void *ctx);

/*
 * Profiles: the kinds of terminal
 */
enum {
	K_VT52, K_DEC, K_ANSI_BBS, K_AVATAR, K_RIP, K_PETSCII, K_ATASCII,
	K_ATARI_ST, K_VIEWDATA, K_MINITEL, K_TEK
};

/* what a DEC-style terminal has beyond its level's text */
enum {
	F_COLOR = 1, F_SIXEL = 2, F_PRINTER = 4, F_XTERM = 8, F_TEK = 16,
	F_REGIS = 32
};

/* a Commodore's attribute codes */
enum { AC_NONE, AC_VDC, AC_PLUS4 };

struct vt_profile {
	const char *id, *label, *term, *telnet;
	int kind;
	/* a DEC terminal's */
	int level;
	unsigned features;
	const char *da1, *da2;
	int charset;            /* CS_*, or -1: the user's */
	int home_on_clear;
	/* its cells' bitmap font (MF_*): its machine's ROM; MF_NONE: the
	   view's font */
	int font;
	/* ANSI-BBS: SyncTERM's number for its font, what CSI SP D's font 0
	   goes back to */
	int syncterm;
	/* ANSI-BBS: its sixteen colours over the theme's; a machine: its
	   palette */
	const uint32_t *colors;
	int ncolors;
	/* a machine's screen */
	int columns, rows, default_color, background;
	/* a Commodore's */
	const uint32_t *upper, *lower;
	const uint8_t *ctrl_colors, *cbm_colors;
	int n_ctrl, n_cbm;
	int escapes, attr_codes, tab_lf;
	/* an Atari's */
	const uint32_t *atascii;
	/* what a capture's file is called (".seq"), NULL for the app's */
	const char *capture_ext;
};

extern const struct vt_profile *const vt_profiles[];
extern const int vt_profile_count;
const struct vt_profile *vt_profile_by_id(const char *id);
const struct vt_profile *vt_profile_default(void);
/* the VT100 a Minitel's Téléinformatique mode is */
extern const struct vt_profile vt_profile_vt100;
/* the interpreter for a profile */
struct vt_interp *profile_interp(dmc_vt *t, const struct vt_profile *p);

/*
 * Keys (vt_keys.c): what a key sends as a VT220/xterm
 */
void keys_key(const dmc_vt *t, int key, int mods, dmc_buf *out);
void keys_char(uint32_t cp, int mods, dmc_buf *out);

/*
 * Graphics (vt_gfx.c)
 */
struct sixel;
struct sixel *sixel_new(int transparent, uint32_t *shared, int max_w,
			int max_h);
void sixel_feed(struct sixel *s, int32_t cp);
struct vt_image *sixel_finish(dmc_vt *t, struct sixel *s, int cw, int ch);
void sixel_free(struct sixel *s);
uint32_t sixel_hls(int h, int l, int s);
void sixel_default_palette(uint32_t p[256]);

/* a graphics terminal's picture: logical coordinates into pixels */
enum { R_OVERLAY, R_REPLACE, R_COMPLEMENT, R_ERASE };
struct raster {
	int width, height, lw, lh;
	uint32_t *px;
	int mode;
	uint32_t color, background;
	int pattern;
	const uint8_t *fill_pattern;    /* 8 rows, NULL solid */
	uint8_t fill_pattern_buf[8];
	uint32_t fill_background;
	int clip[4], has_clip;
	int pattern_step;
	int dirty;
};
struct raster *raster_new(int w, int h, int lw, int lh);
void raster_free(struct raster *r);
int raster_px(const struct raster *r, int x);
int raster_py(const struct raster *r, int y);
void raster_clear(struct raster *r, uint32_t c);
void raster_point(struct raster *r, int x, int y);
void raster_line(struct raster *r, int x0, int y0, int x1, int y1);
void raster_arc(struct raster *r, int cx, int cy, int rad, double start,
		double sweep);
void raster_ellipse(struct raster *r, int cx, int cy, int rx, int ry,
		    double start, double sweep);
/* an elliptical arc's points (logical) into xy (pairs): how many */
int raster_ellipse_points(int cx, int cy, int rx, int ry, double start,
			  double sweep, int *xy, int max);
void raster_fill(struct raster *r, const int *xy, int n);
void raster_flood(struct raster *r, int x, int y, uint32_t border);
struct raster_grab {
	int width, height;
	uint32_t *px;
};
void raster_grab(const struct raster *r, int x0, int y0, int x1, int y1,
		 struct raster_grab *g);
void raster_put(struct raster *r, const struct raster_grab *g, int x, int y,
		int xor_);
void raster_pixel_rect(const struct raster *r, int x0, int y0, int x1, int y1,
		       int out[4]);
void raster_clear_rect(struct raster *r, int x0, int y0, int x1, int y1,
		       uint32_t c);
int raster_text(struct raster *r, int x, int y, const uint32_t *cps, int n,
		const struct vt_font *f, int cw, int ch);
struct vt_image *raster_take(dmc_vt *t, struct raster *r, int cw, int ch);

/* a picture kept the screen's size, put on it when drawn on */
struct picture_layer {
	dmc_vt *t;
	int lw, lh;
	struct raster *r;
	void (*setup)(struct raster *, void *);
	void *ctx;
};
void layer_init(struct picture_layer *l, dmc_vt *t, int lw, int lh,
		void (*setup)(struct raster *, void *), void *ctx);
struct raster *layer_raster(struct picture_layer *l);
void layer_reset(struct picture_layer *l);
void layer_flush(struct picture_layer *l);
void layer_free(struct picture_layer *l);

/* ReGIS (vt_regis.c) */
struct regis;
struct regis *regis_new(dmc_vt *t, uint32_t *registers);
void regis_begin(struct regis *g, int p1);
void regis_feed(struct regis *g, int32_t cp);
void regis_end(struct regis *g);
void regis_abort(struct regis *g);
void regis_flush(struct regis *g);
void regis_reset(struct regis *g);
void regis_free(struct regis *g);

/* RIPscrip (vt_rip.c), over ANSI-BBS: what it doesn't take given back
   to ANSI-BBS's parser through replay */
struct rip;
struct rip *rip_new(dmc_vt *t, void (*replay)(void *, int32_t), void *ctx);
/* a code point to RIP: 1 if it took it */
int rip_intercept(struct rip *r, int32_t cp);
int rip_tap(struct rip *r, double fx, double fy, dmc_buf *out);
void rip_flush(struct rip *r);
void rip_reset(struct rip *r);
void rip_free(struct rip *r);
/* ESC [ ! : RIP's version asked */
void rip_query(struct rip *r);

/* ANSI music (vt_dec.c) */
struct ansi_music {
	int tempo, octave, length;
	double sounding;
};
void music_reset(struct ansi_music *m);
void music_play(dmc_vt *t, struct ansi_music *m, const char *s);

#endif
