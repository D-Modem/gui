/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * zstd_codec.c - Zstandard (RFC 8878), whole frames: see zstd_codec.h.
 *                The same steps as the app's Zstd.kt, so that the two
 *                make the same frames.
 */
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#include "zstd_codec.h"

#define MAX_BLOCK (128 * 1024)
#define HUF_MAX 11

typedef unsigned char u8;
typedef int64_t i64;
typedef uint64_t u64;

static int hbit(unsigned v)
{
	int b = -1;
	while (v) {
		b++;
		v >>= 1;
	}
	return b;
}

static const int LL_BASE[36] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18,
	20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536 };
static const int LL_BITS[36] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2,
	3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
static const int ML_BASE[53] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
	21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 37, 39, 41, 43, 47, 51, 59, 67, 83,
	99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539 };
static const int ML_BITS[53] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
static const int LL_DEF[36] = { 4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 2,
	2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1 };
static const int ML_DEF[53] = { 1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1 };
static const int OF_DEF[29] = { 1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, -1, -1, -1, -1, -1 };

/* ------------------------------------------------------------------ */
/* a growing byte buffer */

typedef struct {
	u8 *p;
	int n, cap;
	int oom;
} zb;

static void zb_room(zb *b, int more)
{
	if (b->oom)
		return;
	if (b->n + more > b->cap) {
		int cap = b->cap ? b->cap : 256;
		while (cap < b->n + more)
			cap *= 2;
		u8 *q = realloc(b->p, (size_t)cap);
		if (!q) {
			b->oom = 1;
			return;
		}
		b->p = q;
		b->cap = cap;
	}
}

static void zb_put(zb *b, int c)
{
	zb_room(b, 1);
	if (!b->oom)
		b->p[b->n++] = (u8)c;
}

static void zb_add(zb *b, const u8 *s, int n)
{
	if (n <= 0)
		return;
	zb_room(b, n);
	if (!b->oom) {
		memcpy(b->p + b->n, s, (size_t)n);
		b->n += n;
	}
}

static void zb_free(zb *b)
{
	free(b->p);
	memset(b, 0, sizeof *b);
}

/* ------------------------------------------------------------------ */
/* bits */

/* a forward bit stream (FSE table descriptions), the lowest bits first */
typedef struct {
	const u8 *p;
	int n;
	i64 pos;
	int bad;
} fwd_bits;

static int fwd_peek(fwd_bits *f, int nb)
{
	int v = 0;
	for (int k = 0; k < nb; k++) {
		i64 q = f->pos + k;
		if (q < 8LL * f->n)
			v |= ((f->p[q >> 3] >> (q & 7)) & 1) << k;
	}
	return v;
}

static int fwd_read(fwd_bits *f, int nb)
{
	int v = fwd_peek(f, nb);
	f->pos += nb;
	if (f->pos > 8LL * f->n)
		f->bad = 1;
	return v;
}

/* a backward bit stream (Huffman and FSE coded data): from under the
   last byte's marker toward the start */
typedef struct {
	const u8 *p;
	int n;
	i64 off;
} back_bits;

static int back_init(back_bits *b, const u8 *p, int n)
{
	b->p = p;
	b->n = n;
	if (n <= 0 || p[n - 1] == 0)
		return 0;
	b->off = 8LL * (n - 1) + hbit(p[n - 1]);
	return 1;
}

static u64 back_read(back_bits *b, int nb)
{
	u64 v = 0;
	b->off -= nb;
	for (int k = nb - 1; k >= 0; k--) {
		i64 q = b->off + k;
		v = (v << 1) | (q >= 0 ? (u64)((b->p[q >> 3] >> (q & 7)) & 1) : 0);
	}
	return v;
}

/* a forward bit writer, the lowest bits first */
typedef struct {
	zb b;
	int acc, n;
} bit_w;

static void bw_put(bit_w *w, u64 v, int nb)
{
	for (int k = 0; k < nb; k++) {
		w->acc |= (int)((v >> k) & 1) << w->n;
		if (++w->n == 8) {
			zb_put(&w->b, w->acc);
			w->acc = 0;
			w->n = 0;
		}
	}
}

static void bw_close(bit_w *w)
{
	bw_put(w, 1, 1);
	if (w->n != 0)
		bw_put(w, 0, 8 - w->n);
}

/* ------------------------------------------------------------------ */
/* tables */

#define FSE_MAX 512                     /* 1 << 9 */
#define HUF_SIZE (1 << HUF_MAX)

/* an FSE decoding table: each state's symbol, bits and baseline */
typedef struct {
	int log;
	int sym[FSE_MAX], nb[FSE_MAX], base[FSE_MAX];
} fse_t;

/* a Huffman decoding table: by max_bits bits, the symbol and its length */
typedef struct {
	int max_bits;                   /* 0: none */
	u8 sym[HUF_SIZE], nb[HUF_SIZE];
} huf_t;

/* an FSE table from the counts (-1: "less than 1") at accuracy log al
   (4.1.1) */
static int fse_build(fse_t *t, const int *cnt, int nsym, int al)
{
	int size = 1 << al, high = size - 1;
	int next[256];
	if (al > 9 || nsym > 256)
		return 0;
	t->log = al;
	for (int s = 0; s < nsym; s++) {
		if (cnt[s] == -1) {
			if (high < 0)
				return 0;
			t->sym[high--] = s;
			next[s] = 1;
		} else
			next[s] = cnt[s] > 0 ? cnt[s] : 0;
	}
	int step = (size >> 1) + (size >> 3) + 3, mask = size - 1, pos = 0;
	for (int s = 0; s < nsym; s++)
		for (int i = 0; i < cnt[s]; i++) {
			if (pos > high)
				return 0;
			t->sym[pos] = s;
			do
				pos = (pos + step) & mask;
			while (pos > high);
		}
	if (pos != 0)
		return 0;
	for (int i = 0; i < size; i++) {
		int d = next[t->sym[i]]++;
		if (d <= 0)
			return 0;
		int nb = al - hbit((unsigned)d);
		t->nb[i] = nb;
		t->base[i] = (d << nb) - size;
	}
	return 1;
}

/* an FSE table description at p[0..n): the counts (*nc of them) and the
   accuracy log, and its bytes; -1 if bad */
static int fse_desc(const u8 *p, int n, int max_al, int max_sym, int *cnt, int *nc, int *al_out)
{
	if (n < 1)
		return -1;
	fwd_bits in = { p, n, 0, 0 };
	int al = 5 + fwd_read(&in, 4);
	if (al > max_al)
		return -1;
	int remaining = 1 << al, bits = al + 1, k = 0;
	while (remaining > 0 && k < max_sym) {
		int lower = (1 << (bits - 1)) - 1;
		int thr = (1 << bits) - 1 - (remaining + 1);
		int v;
		if ((fwd_peek(&in, bits - 1) & lower) < thr) {
			v = fwd_read(&in, bits - 1);
		} else {
			v = fwd_read(&in, bits);
			if (v > lower)
				v -= thr;
		}
		int c = v - 1;
		remaining -= c < 0 ? -c : c;
		cnt[k++] = c;
		if (c == 0) {
			for (;;) {
				int rep = fwd_read(&in, 2);
				for (int j = 0; j < rep && k < max_sym; j++)
					cnt[k++] = 0;
				if (rep != 3 || in.bad)
					break;
			}
		}
		if (remaining < 0)
			return -1;
		bits = hbit((unsigned)remaining + 1) + 1;
		if (in.bad)
			return -1;
	}
	if (remaining != 0 || in.bad)
		return -1;
	*nc = k;
	*al_out = al;
	return (int)((in.pos + 7) / 8);
}

/* a Huffman table from the weights of symbols 0..nw-1, the last's (nw)
   implied (4.2.1) */
static int huf_build(huf_t *h, const int *w, int nw)
{
	int sum = 0;
	for (int i = 0; i < nw; i++) {
		if (w[i] > HUF_MAX + 1 || w[i] < 0)
			return 0;
		if (w[i] != 0)
			sum += 1 << (w[i] - 1);
	}
	if (sum == 0 || nw > 255)
		return 0;
	int mb = hbit((unsigned)sum) + 1;
	int left = (1 << mb) - sum;
	if (mb > HUF_MAX || (left & (left - 1)) != 0)
		return 0;
	int bits[256];
	for (int s = 0; s < nw; s++)
		bits[s] = w[s] != 0 ? mb + 1 - w[s] : 0;
	bits[nw] = mb + 1 - (hbit((unsigned)left) + 1);
	int rank_cnt[HUF_MAX + 2] = { 0 }, rank_idx[HUF_MAX + 2] = { 0 };
	for (int s = 0; s <= nw; s++)
		if (bits[s] != 0)
			rank_cnt[bits[s]]++;
	rank_idx[mb] = 0;
	for (int i = mb; i >= 1; i--)
		rank_idx[i - 1] = rank_idx[i] + rank_cnt[i] * (1 << (mb - i));
	h->max_bits = mb;
	for (int s = 0; s <= nw; s++) {
		if (bits[s] == 0)
			continue;
		int len = 1 << (mb - bits[s]);
		int at = rank_idx[bits[s]];
		if (at + len > (1 << mb))
			return 0;
		for (int k = 0; k < len; k++) {
			h->sym[at + k] = (u8)s;
			h->nb[at + k] = (u8)bits[s];
		}
		rank_idx[bits[s]] += len;
	}
	return 1;
}

/* a Huffman tree description at p[0..n): its bytes, or -1 */
static int huf_desc(const u8 *p, int n, huf_t *h)
{
	int w[256], nw = 0, used;
	if (n < 1)
		return -1;
	int h0 = p[0];
	if (h0 >= 128) {
		int cnt = h0 - 127;
		used = 1 + (cnt + 1) / 2;
		if (used > n)
			return -1;
		for (int i = 0; i < cnt; i++) {
			int b = p[1 + i / 2];
			w[nw++] = (i & 1) ? b & 15 : b >> 4;
		}
	} else {
		int cs = h0, cnt[256], nc, al;
		used = 1 + cs;
		if (used > n || cs == 0)
			return -1;
		int d = fse_desc(p + 1, cs, 6, 255, cnt, &nc, &al);
		if (d < 0 || d >= cs)
			return -1;
		fse_t *t = malloc(sizeof *t);
		if (!t)
			return -1;
		back_bits in;
		if (!fse_build(t, cnt, nc, al) || !back_init(&in, p + 1 + d, cs - d)) {
			free(t);
			return -1;
		}
		int s1 = (int)back_read(&in, al), s2 = (int)back_read(&in, al);
		for (;;) {
			if (nw > 253) {
				free(t);
				return -1;
			}
			w[nw++] = t->sym[s1];
			s1 = t->base[s1] + (int)back_read(&in, t->nb[s1]);
			if (in.off < 0) {
				w[nw++] = t->sym[s2];
				break;
			}
			w[nw++] = t->sym[s2];
			s2 = t->base[s2] + (int)back_read(&in, t->nb[s2]);
			if (in.off < 0) {
				w[nw++] = t->sym[s1];
				break;
			}
		}
		free(t);
	}
	return huf_build(h, w, nw) ? used : -1;
}

/* a Huffman coded stream into out[0..count); 0 if bad */
static int huf_stream(const huf_t *h, const u8 *p, int n, int count, u8 *out)
{
	back_bits in;
	if (!back_init(&in, p, n))
		return 0;
	int mask = (1 << h->max_bits) - 1;
	int st = (int)back_read(&in, h->max_bits);
	for (int k = 0; k < count; k++) {
		out[k] = h->sym[st];
		int nb = h->nb[st];
		st = ((st << nb) & mask) | (int)back_read(&in, nb);
	}
	return in.off + h->max_bits == 0;
}

static int len_code(int v, const int *base, int n)
{
	int c = 0;
	while (c + 1 < n && base[c + 1] <= v)
		c++;
	return c;
}

/* the repeat offsets after a sequence's offset value (3.2.2.3): its
   offset */
static i64 apply_offset(i64 *rep, i64 ov, int ll)
{
	if (ov > 3) {
		rep[2] = rep[1];
		rep[1] = rep[0];
		rep[0] = ov - 3;
		return rep[0];
	}
	int idx = (int)ov - 1 + (ll == 0 ? 1 : 0);
	if (idx == 0)
		return rep[0];
	i64 off = idx == 3 ? rep[0] - 1 : rep[idx];
	if (idx > 1)
		rep[2] = rep[1];
	rep[1] = rep[0];
	rep[0] = off;
	return off;
}

/* ------------------------------------------------------------------ */
/* the encoder's tables */

/* an FSE encoder: each symbol's states in order */
typedef struct {
	int al, nsym;
	int cnt[53];
	int first[54];
	int states[FSE_MAX];
} fse_enc;

static void fse_enc_init(fse_enc *e, const int *c, int nsym, int al)
{
	fse_t t;
	int fill[53];
	e->al = al;
	e->nsym = nsym;
	memcpy(e->cnt, c, sizeof(int) * (size_t)nsym);
	fse_build(&t, c, nsym, al);
	e->first[0] = 0;
	for (int s = 0; s < nsym; s++) {
		int k = c[s] < 0 ? 1 : c[s];
		e->first[s + 1] = e->first[s] + k;
		fill[s] = e->first[s];
	}
	for (int i = 0; i < (1 << al); i++)
		e->states[fill[t.sym[i]]++] = i;
}

static int fse_enc_step(const fse_enc *e, int st, int s, bit_w *w)
{
	int c = e->cnt[s] < 0 ? 1 : e->cnt[s];
	int x = st + (1 << e->al);
	int nb = hbit((unsigned)x) - hbit((unsigned)c);
	if ((x >> nb) < c)
		nb--;
	int d = x >> nb;
	bw_put(w, (u64)(x & ((1 << nb) - 1)), nb);
	return e->states[e->first[s] + d - c];
}

/* Huffman code lengths of the counts, none over HUF_MAX; 0 with fewer
   than two symbols */
static int huf_lengths(const int *freq, int *bits)
{
	int f[256];
	i64 w[512];
	int l[512], r[512], live[256], stk_i[512], stk_d[512];
	memcpy(f, freq, sizeof f);
	for (;;) {
		int nn = 0, nl = 0;
		for (int s = 0; s < 256; s++)
			if (f[s] != 0) {
				w[nn] = f[s];
				l[nn] = -1;
				r[nn] = s;
				live[nl++] = nn++;
			}
		if (nl < 2)
			return 0;
		/* the two lightest (the earlier on a tie) */
		while (nl > 1) {
			int pick[2];
			for (int j = 0; j < 2; j++) {
				int ia = 0;
				for (int k = 1; k < nl; k++) {
					int a = live[k], b = live[ia];
					if (w[a] != w[b] ? w[a] < w[b] : a < b)
						ia = k;
				}
				pick[j] = live[ia];
				memmove(live + ia, live + ia + 1, sizeof(int) * (size_t)(nl - ia - 1));
				nl--;
			}
			w[nn] = w[pick[0]] + w[pick[1]];
			l[nn] = pick[0];
			r[nn] = pick[1];
			live[nl++] = nn++;
		}
		int mx = 0, sp = 0;
		memset(bits, 0, sizeof(int) * 256);
		stk_i[sp] = live[0];
		stk_d[sp++] = 0;
		while (sp > 0) {
			sp--;
			int i = stk_i[sp], d = stk_d[sp];
			if (l[i] < 0) {
				bits[r[i]] = d;
				if (d > mx)
					mx = d;
			} else {
				stk_i[sp] = l[i];
				stk_d[sp++] = d + 1;
				stk_i[sp] = r[i];
				stk_d[sp++] = d + 1;
			}
		}
		if (mx <= HUF_MAX)
			return 1;
		for (int s = 0; s < 256; s++)
			if (f[s] != 0)
				f[s] = (f[s] + 1) / 2;
	}
}

/* the codes of the lengths, as the decoder's table has them */
static void huf_codes(const int *bits, int *code)
{
	int mb = 0;
	int rank_cnt[HUF_MAX + 2] = { 0 }, rank_idx[HUF_MAX + 2] = { 0 };
	for (int s = 0; s < 256; s++)
		if (bits[s] > mb)
			mb = bits[s];
	for (int s = 0; s < 256; s++)
		if (bits[s] != 0)
			rank_cnt[bits[s]]++;
	rank_idx[mb] = 0;
	for (int i = mb; i >= 1; i--)
		rank_idx[i - 1] = rank_idx[i] + rank_cnt[i] * (1 << (mb - i));
	for (int s = 0; s < 256; s++) {
		code[s] = 0;
		if (bits[s] == 0)
			continue;
		code[s] = rank_idx[bits[s]] >> (mb - bits[s]);
		rank_idx[bits[s]] += 1 << (mb - bits[s]);
	}
}

static void huf_encode(const u8 *lit, int from, int to, const int *bits, const int *code, zb *o)
{
	bit_w w = { { 0 }, 0, 0 };
	for (int k = to - 1; k >= from; k--)
		bw_put(&w, (u64)code[lit[k]], bits[lit[k]]);
	bw_close(&w);
	zb_add(o, w.b.p, w.b.n);
	if (w.b.oom)
		o->oom = 1;
	zb_free(&w.b);
}

static void lit_header(zb *o, int type, int regen, int comp, int one_stream)
{
	if (type < 2) {
		if (regen < 32) {
			zb_put(o, type | (regen << 3));
		} else if (regen < 4096) {
			zb_put(o, type | (1 << 2) | ((regen & 15) << 4));
			zb_put(o, (regen >> 4) & 0xff);
		} else {
			zb_put(o, type | (3 << 2) | ((regen & 15) << 4));
			zb_put(o, (regen >> 4) & 0xff);
			zb_put(o, (regen >> 12) & 0xff);
		}
		return;
	}
	int sf, nb, hs;
	if (one_stream) {
		sf = 0; nb = 10; hs = 3;
	} else if (regen < 1024 && comp < 1024) {
		sf = 1; nb = 10; hs = 3;
	} else if (regen < 16384 && comp < 16384) {
		sf = 2; nb = 14; hs = 4;
	} else {
		sf = 3; nb = 18; hs = 5;
	}
	u64 h = (u64)type | ((u64)sf << 2) | ((u64)regen << 4) | ((u64)comp << (4 + nb));
	for (int k = 0; k < hs; k++)
		zb_put(o, (int)((h >> (8 * k)) & 0xff));
}

/* the literals coded (one stream under 1024, else four and the jump
   table) into o; 0 if a stream is too large */
static int huf_section(const u8 *lit, int n, const int *bits, zb *o)
{
	int code[256];
	huf_codes(bits, code);
	if (n < 1024) {
		huf_encode(lit, 0, n, bits, code, o);
		return 1;
	}
	int seg = (n + 3) / 4;
	zb s[4];
	memset(s, 0, sizeof s);
	for (int k = 0; k < 4; k++) {
		int a = k * seg < n ? k * seg : n, b = (k + 1) * seg < n ? (k + 1) * seg : n;
		huf_encode(lit, a, b, bits, code, &s[k]);
	}
	int ok = 1;
	for (int k = 0; k < 3; k++) {
		if (s[k].n > 65535)
			ok = 0;
		zb_put(o, s[k].n & 0xff);
		zb_put(o, (s[k].n >> 8) & 0xff);
	}
	for (int k = 0; k < 4; k++) {
		zb_add(o, s[k].p, s[k].n);
		if (s[k].oom)
			o->oom = 1;
		zb_free(&s[k]);
	}
	return ok;
}

static void fse_normalize(const int *f, int nsym, int al, int *c)
{
	i64 total = 0;
	int size = 1 << al, sum = 0, big = 0;
	for (int s = 0; s < nsym; s++)
		total += f[s];
	for (int s = 0; s < nsym; s++) {
		c[s] = 0;
		if (f[s] == 0)
			continue;
		i64 v = ((i64)f[s] * size + total / 2) / total;
		c[s] = v < 1 ? 1 : (int)v;
		sum += c[s];
		if (c[s] > c[big])
			big = s;
	}
	c[big] += size - sum;
	while (c[big] < 1) {
		int need = 1 - c[big];
		c[big] = 1;
		for (int s = 0; s < nsym; s++) {
			while (need > 0 && s != big && c[s] > 1) {
				c[s]--;
				need--;
			}
			if (need == 0)
				break;
		}
	}
}

static void fse_desc_write(const int *c, int n, int al, zb *o)
{
	bit_w w = { { 0 }, 0, 0 };
	bw_put(&w, (u64)(al - 5), 4);
	int remaining = 1 << al, s = 0;
	while (s < n && remaining > 0) {
		int bits = hbit((unsigned)remaining + 1) + 1;
		int lower = (1 << (bits - 1)) - 1;
		int thr = (1 << bits) - 1 - (remaining + 1);
		int v = c[s] + 1;
		if (v < thr)
			bw_put(&w, (u64)v, bits - 1);
		else if (v <= lower)
			bw_put(&w, (u64)v, bits);
		else
			bw_put(&w, (u64)(v + thr), bits);
		remaining -= c[s] < 0 ? -c[s] : c[s];
		s++;
		if (c[s - 1] == 0) {
			int z = 0;
			while (s + z < n && c[s + z] == 0)
				z++;
			s += z;
			while (z >= 3) {
				bw_put(&w, 3, 2);
				z -= 3;
			}
			bw_put(&w, (u64)z, 2);
		}
	}
	if (w.n != 0)
		bw_put(&w, 0, 8 - w.n);
	zb_add(o, w.b.p, w.b.n);
	if (w.b.oom)
		o->oom = 1;
	zb_free(&w.b);
}

static double fse_cost(const int *cnt, int ncnt, int al, const int *sym, int ns)
{
	double bits = 0;
	for (int k = 0; k < ns; k++) {
		int x = sym[k];
		if (x >= ncnt || cnt[x] == 0)
			return -1;
		int c = cnt[x] < 0 ? 1 : cnt[x];
		int hb = hbit((unsigned)c);
		bits += al - (hb + (double)(c - (1 << hb)) / (1 << hb));
	}
	return bits;
}

/* ------------------------------------------------------------------ */
/* the encoder */

struct enc {
	const u8 *hist;
	int n, window;
	i64 rep[3];
	int head[1 << 14];
	int *chain;
	int ins;
	int enc_bits[256], has_enc_bits;
	zb out;
	int match_off;
	/* a block's sequences */
	int *sq_ll, *sq_ov, *sq_ml, *llc, *ofc, *mlc;
};

static int hash3(const u8 *h, int i)
{
	uint32_t v = ((uint32_t)h[i] << 16) | ((uint32_t)h[i + 1] << 8) | h[i + 2];
	return (int)((v * 2654435761u) >> 18);
}

static void enc_insert(struct enc *e, int i)
{
	if (i + 2 >= e->n)
		return;
	int h = hash3(e->hist, i);
	e->chain[i] = e->head[h];
	e->head[h] = i + 1;
}

static int len_at(const u8 *h, int i, int d, int lim)
{
	int l = 0;
	while (l < lim && h[i - d + l] == h[i + l])
		l++;
	return l;
}

static int best_match(struct enc *e, int i, int end)
{
	int lim = end - i < 65536 ? end - i : 65536, best = 0;
	for (int r = 0; r < 3; r++) {
		i64 d = e->rep[r];
		if (d <= 0 || d > i || d > e->window)
			continue;
		int l = len_at(e->hist, i, (int)d, lim);
		if (l > best) {
			best = l;
			e->match_off = (int)d;
		}
	}
	int rep_best = best;
	if (lim >= 3) {
		int c = e->head[hash3(e->hist, i)], depth = 48;
		while (c != 0 && depth-- > 0) {
			int cand = c - 1, d = i - cand;
			if (d > e->window || d <= 0)
				break;
			int l = len_at(e->hist, i, d, lim);
			if (l > best && (l > rep_best + 1 || best < 3) && (l > 3 || d <= 1024)) {
				best = l;
				e->match_off = d;
			}
			if (best == lim)
				break;
			c = e->chain[cand];
		}
	}
	return best >= 3 ? best : 0;
}

static void put_block(struct enc *e, const u8 *body, int body_n, int type, int size, int last)
{
	int h = (last ? 1 : 0) | (type << 1) | (size << 3);
	zb_put(&e->out, h & 0xff);
	zb_put(&e->out, (h >> 8) & 0xff);
	zb_put(&e->out, (h >> 16) & 0xff);
	zb_add(&e->out, body, body_n);
}

static void enc_catch_up(struct enc *e, int upto)
{
	while (e->ins < upto) {
		enc_insert(e, e->ins);
		e->ins++;
	}
}

static void enc_block(struct enc *e, int start, int end, int last)
{
	const u8 *hist = e->hist;
	int nb = end - start;
	i64 rep_saved[3];
	memcpy(rep_saved, e->rep, sizeof rep_saved);
	int ns = 0;
	zb lit = { 0 }, best_lit = { 0 }, body = { 0 };
	int i = start, lit_at = start;
	if (e->ins < start)
		e->ins = start;
	while (i + 3 <= end) {
		enc_catch_up(e, i);
		int len = best_match(e, i, end);
		int off = e->match_off;
		if (len != 0 && i + 1 + 3 <= end) {
			enc_catch_up(e, i + 1);
			int len2 = best_match(e, i + 1, end);
			if (len2 > len + 1) {
				i++;
				continue;
			}
		}
		if (len == 0) {
			i++;
			continue;
		}
		int ll = i - lit_at;
		int ov;
		if (ll != 0)
			ov = off == e->rep[0] ? 1 : off == e->rep[1] ? 2 : off == e->rep[2] ? 3 : off + 3;
		else
			ov = off == e->rep[1] ? 1 : off == e->rep[2] ? 2 : off == e->rep[0] - 1 ? 3 : off + 3;
		apply_offset(e->rep, ov, ll);
		zb_add(&lit, hist + lit_at, i - lit_at);
		e->sq_ll[ns] = ll;
		e->sq_ov[ns] = ov;
		e->sq_ml[ns] = len;
		ns++;
		i += len;
		lit_at = i;
	}
	zb_add(&lit, hist + lit_at, end - lit_at);
	enc_catch_up(e, e->ins > end - 2 ? e->ins : end - 2);

	/* the literals: raw, RLE, Huffman with a new table or the last */
	int nl = lit.n, freq[256] = { 0 }, kinds = 0, last_sym = 0;
	int new_bits[256], has_new_bits = 0;
	for (int k = 0; k < nl; k++)
		freq[lit.p[k]]++;
	for (int s = 0; s < 256; s++)
		if (freq[s] != 0) {
			kinds++;
			last_sym = s;
		}
	lit_header(&best_lit, 0, nl, 0, 0);
	zb_add(&best_lit, lit.p, nl);
	if (kinds == 1 && nl > 1) {
		zb r = { 0 };
		lit_header(&r, 1, nl, 0, 0);
		zb_put(&r, lit.p[0]);
		if (r.n < best_lit.n) {
			zb_free(&best_lit);
			best_lit = r;
		} else
			zb_free(&r);
	} else if (kinds > 1 && nl >= 16) {
		int bits[256];
		if (last_sym <= 128 && huf_lengths(freq, bits)) {
			int mb = 0;
			zb desc = { 0 }, st = { 0 };
			for (int s = 0; s < 256; s++)
				if (bits[s] > mb)
					mb = bits[s];
			zb_put(&desc, 127 + last_sym);
			for (int s = 0; s < last_sym; s += 2) {
				int w0 = bits[s] != 0 ? mb + 1 - bits[s] : 0;
				int w1 = s + 1 < last_sym && bits[s + 1] != 0 ? mb + 1 - bits[s + 1] : 0;
				zb_put(&desc, (w0 << 4) | w1);
			}
			if (huf_section(lit.p, nl, bits, &st)) {
				int comp = desc.n + st.n;
				if (!(nl < 1024 && comp >= 1024)) {
					zb r = { 0 };
					lit_header(&r, 2, nl, comp, nl < 1024);
					zb_add(&r, desc.p, desc.n);
					zb_add(&r, st.p, st.n);
					if (r.n < best_lit.n && !r.oom) {
						zb_free(&best_lit);
						best_lit = r;
						memcpy(new_bits, bits, sizeof new_bits);
						has_new_bits = 1;
					} else
						zb_free(&r);
				}
			}
			zb_free(&desc);
			zb_free(&st);
		}
		int fits = e->has_enc_bits;
		for (int s = 0; fits && s < 256; s++)
			if (freq[s] != 0 && e->enc_bits[s] == 0)
				fits = 0;
		if (fits) {
			zb st = { 0 };
			if (huf_section(lit.p, nl, e->enc_bits, &st) && !(nl < 1024 && st.n >= 1024)) {
				zb r = { 0 };
				lit_header(&r, 3, nl, st.n, nl < 1024);
				zb_add(&r, st.p, st.n);
				if (r.n < best_lit.n && !r.oom) {
					zb_free(&best_lit);
					best_lit = r;
					has_new_bits = 0;
				} else
					zb_free(&r);
			}
			zb_free(&st);
		}
	}
	zb_add(&body, best_lit.p, best_lit.n);

	/* the sequences */
	if (ns < 128) {
		zb_put(&body, ns);
	} else if (ns < 0x7F00) {
		zb_put(&body, (ns >> 8) + 128);
		zb_put(&body, ns & 0xff);
	} else {
		zb_put(&body, 255);
		zb_put(&body, (ns - 0x7F00) & 0xff);
		zb_put(&body, (ns - 0x7F00) >> 8);
	}
	if (ns != 0) {
		int *llc = e->llc, *mlc = e->mlc, *ofc = e->ofc;
		for (int k = 0; k < ns; k++) {
			llc[k] = len_code(e->sq_ll[k], LL_BASE, 36);
			mlc[k] = len_code(e->sq_ml[k], ML_BASE, 53);
			ofc[k] = hbit((unsigned)e->sq_ov[k]);
		}
		const int *codes[3] = { llc, ofc, mlc };
		const int *defs[3] = { LL_DEF, OF_DEF, ML_DEF };
		const int ndefs[3] = { 36, 29, 53 };
		const int dal[3] = { 6, 5, 6 }, max_al[3] = { 9, 8, 9 };
		fse_enc enc[3];
		int modes = 0;
		zb descs = { 0 };
		for (int t = 0; t < 3; t++) {
			double cp = fse_cost(defs[t], ndefs[t], dal[t], codes[t], ns);
			int f[53] = { 0 }, nsym = 0, k2 = 0, nc[53];
			for (int k = 0; k < ns; k++) {
				int x = codes[t][k];
				if (f[x]++ == 0)
					k2++;
				if (x + 1 > nsym)
					nsym = x + 1;
			}
			int al = hbit((unsigned)ns) - 1;
			if (al < 5)
				al = 5;
			if (hbit((unsigned)k2) + 1 > al)
				al = hbit((unsigned)k2) + 1;
			if (al > max_al[t])
				al = max_al[t];
			fse_normalize(f, nsym, al, nc);
			zb d = { 0 };
			fse_desc_write(nc, nsym, al, &d);
			double cn = 8.0 * d.n + fse_cost(nc, nsym, al, codes[t], ns);
			if (cp >= 0 && cp <= cn) {
				fse_enc_init(&enc[t], defs[t], ndefs[t], dal[t]);
			} else {
				modes |= 2 << (6 - 2 * t);
				zb_add(&descs, d.p, d.n);
				fse_enc_init(&enc[t], nc, nsym, al);
			}
			zb_free(&d);
		}
		zb_put(&body, modes);
		zb_add(&body, descs.p, descs.n);
		if (descs.oom)
			body.oom = 1;
		zb_free(&descs);
		const fse_enc *fl = &enc[0], *fo = &enc[1], *fm = &enc[2];
		bit_w w = { { 0 }, 0, 0 };
		int k = ns - 1;
		int sl = fl->states[fl->first[llc[k]]];
		int sm = fm->states[fm->first[mlc[k]]];
		int so = fo->states[fo->first[ofc[k]]];
		for (;;) {
			bw_put(&w, (u64)(e->sq_ll[k] - LL_BASE[llc[k]]), LL_BITS[llc[k]]);
			bw_put(&w, (u64)(e->sq_ml[k] - ML_BASE[mlc[k]]), ML_BITS[mlc[k]]);
			bw_put(&w, (u64)(e->sq_ov[k] - (1 << ofc[k])), ofc[k]);
			if (--k < 0)
				break;
			so = fse_enc_step(fo, so, ofc[k], &w);
			sm = fse_enc_step(fm, sm, mlc[k], &w);
			sl = fse_enc_step(fl, sl, llc[k], &w);
		}
		bw_put(&w, (u64)sm, fm->al);
		bw_put(&w, (u64)so, fo->al);
		bw_put(&w, (u64)sl, fl->al);
		bw_close(&w);
		zb_add(&body, w.b.p, w.b.n);
		if (w.b.oom)
			body.oom = 1;
		zb_free(&w.b);
	}
	if (lit.oom || best_lit.oom || body.oom)
		e->out.oom = 1;

	u8 first = hist[start];
	int same = 1, coded = 0;
	for (int k = start; k < end; k++)
		if (hist[k] != first) {
			same = 0;
			break;
		}
	if (same && nb > 1) {
		put_block(e, &first, 1, 1, nb, last);
	} else if (body.n < nb) {
		put_block(e, body.p, body.n, 2, body.n, last);
		if (has_new_bits) {
			memcpy(e->enc_bits, new_bits, sizeof new_bits);
			e->has_enc_bits = 1;
		}
		coded = 1;
	} else {
		put_block(e, hist + start, nb, 0, nb, last);
	}
	/* (the decoder saw no sequences of a block not coded: its offsets as
	   before) */
	if (!coded)
		memcpy(e->rep, rep_saved, sizeof rep_saved);
	zb_free(&lit);
	zb_free(&best_lit);
	zb_free(&body);
}

unsigned char *zc_encode(const unsigned char *data, int n, int *out_n)
{
	struct enc *e = calloc(1, sizeof *e);
	unsigned char *res = NULL;
	int blk = n < MAX_BLOCK ? n : MAX_BLOCK;
	if (!e)
		return NULL;
	e->hist = data;
	e->n = n;
	e->window = n > 1 ? n : 1;
	e->rep[0] = 1;
	e->rep[1] = 4;
	e->rep[2] = 8;
	e->chain = malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
	e->sq_ll = malloc(sizeof(int) * (size_t)(blk / 3 + 1));
	e->sq_ov = malloc(sizeof(int) * (size_t)(blk / 3 + 1));
	e->sq_ml = malloc(sizeof(int) * (size_t)(blk / 3 + 1));
	e->llc = malloc(sizeof(int) * (size_t)(blk / 3 + 1));
	e->ofc = malloc(sizeof(int) * (size_t)(blk / 3 + 1));
	e->mlc = malloc(sizeof(int) * (size_t)(blk / 3 + 1));
	if (e->chain && e->sq_ll && e->sq_ov && e->sq_ml && e->llc && e->ofc && e->mlc) {
		/* the header: single segment, the checksum, the content size */
		int fcs_flag = n < 256 ? 0 : n < 65536 + 256 ? 1 : 2;
		zb_put(&e->out, 0x28);
		zb_put(&e->out, 0xB5);
		zb_put(&e->out, 0x2F);
		zb_put(&e->out, 0xFD);
		zb_put(&e->out, (fcs_flag << 6) | 0x20 | 0x04);
		if (fcs_flag == 0) {
			zb_put(&e->out, n);
		} else if (fcs_flag == 1) {
			zb_put(&e->out, (n - 256) & 0xff);
			zb_put(&e->out, (n - 256) >> 8);
		} else {
			for (int k = 0; k < 4; k++)
				zb_put(&e->out, (n >> (8 * k)) & 0xff);
		}
		if (n == 0)
			put_block(e, NULL, 0, 0, 0, 1);
		for (int at = 0; at < n;) {
			int len = n - at < MAX_BLOCK ? n - at : MAX_BLOCK;
			enc_block(e, at, at + len, at + len == n);
			at += len;
		}
		u64 sum = zc_xxh64(data, (size_t)n);
		for (int k = 0; k < 4; k++)
			zb_put(&e->out, (int)((sum >> (8 * k)) & 0xff));
		if (!e->out.oom) {
			res = e->out.p;
			*out_n = e->out.n;
			e->out.p = NULL;
		}
	}
	zb_free(&e->out);
	free(e->chain);
	free(e->sq_ll);
	free(e->sq_ov);
	free(e->sq_ml);
	free(e->llc);
	free(e->ofc);
	free(e->mlc);
	free(e);
	return res;
}

/* ------------------------------------------------------------------ */
/* the decoder */

struct dec {
	const u8 *p;
	int max;
	u8 *hist;
	int hn, hcap;
	u8 *lit;                        /* a block's literals */
	int nlit;
	i64 rep[3];
	huf_t huf;
	fse_t ll, of, ml;
	int have[3];
	i64 window;
	jmp_buf jb;
	const char *why;
};

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
static void bad(struct dec *d, const char *why)
{
	d->why = why;
	longjmp(d->jb, 1);
}

static void emit(struct dec *d, u8 c)
{
	if (d->hn >= d->max)
		bad(d, "more content than the limit");
	if (d->hn == d->hcap) {
		int cap = d->hcap * 2 < d->max ? d->hcap * 2 : d->max;
		u8 *q = realloc(d->hist, (size_t)cap);
		if (!q)
			bad(d, "no memory");
		d->hist = q;
		d->hcap = cap;
	}
	d->hist[d->hn++] = c;
}

static int dec_literals(struct dec *d, int at, int n)
{
	const u8 *p = d->p;
	if (n < 1)
		bad(d, "no literals");
	int b0 = p[at], type = b0 & 3, sf = (b0 >> 2) & 3;
	d->nlit = 0;
	if (type < 2) {
		int regen, hs;
		if ((sf & 1) == 0) {
			regen = b0 >> 3;
			hs = 1;
		} else if (sf == 1) {
			if (n < 2)
				bad(d, "bad literals");
			regen = (b0 >> 4) | (p[at + 1] << 4);
			hs = 2;
		} else {
			if (n < 3)
				bad(d, "bad literals");
			regen = (b0 >> 4) | (p[at + 1] << 4) | (p[at + 2] << 12);
			hs = 3;
		}
		if (regen > MAX_BLOCK)
			bad(d, "bad literals");
		if (type == 0) {
			if (hs + regen > n)
				bad(d, "bad literals");
			memcpy(d->lit, p + at + hs, (size_t)regen);
			d->nlit = regen;
			return hs + regen;
		}
		if (hs + 1 > n)
			bad(d, "bad literals");
		memset(d->lit, p[at + hs], (size_t)regen);
		d->nlit = regen;
		return hs + 1;
	}
	int streams = sf == 0 ? 1 : 4;
	int hs = sf < 2 ? 3 : sf == 2 ? 4 : 5;
	int nb = sf < 2 ? 10 : sf == 2 ? 14 : 18;
	if (n < hs)
		bad(d, "bad literals");
	u64 h = 0;
	for (int k = 0; k < hs; k++)
		h |= (u64)p[at + k] << (8 * k);
	int regen = (int)((h >> 4) & ((1u << nb) - 1));
	int comp = (int)((h >> (4 + nb)) & ((1u << nb) - 1));
	if (hs + comp > n || regen > MAX_BLOCK)
		bad(d, "bad literals");
	int q = at + hs, left = comp;
	if (type == 2) {
		int k = huf_desc(p + q, left, &d->huf);
		if (k < 0)
			bad(d, "a bad Huffman table");
		q += k;
		left -= k;
	} else if (d->huf.max_bits == 0)
		bad(d, "no Huffman table");
	if (streams == 1) {
		if (!huf_stream(&d->huf, p + q, left, regen, d->lit))
			bad(d, "bad Huffman data");
	} else {
		if (left < 6)
			bad(d, "bad literals");
		int sz[4];
		sz[0] = p[q] | (p[q + 1] << 8);
		sz[1] = p[q + 2] | (p[q + 3] << 8);
		sz[2] = p[q + 4] | (p[q + 5] << 8);
		sz[3] = left - 6 - sz[0] - sz[1] - sz[2];
		if (sz[3] < 0)
			bad(d, "bad literals");
		int seg = (regen + 3) / 4, o = 0;
		q += 6;
		for (int k = 0; k < 4; k++) {
			int cnt = k < 3 ? seg : regen - 3 * seg;
			if (cnt < 0 || !huf_stream(&d->huf, p + q, sz[k], cnt, d->lit + o))
				bad(d, "bad Huffman data");
			o += cnt;
			q += sz[k];
		}
	}
	d->nlit = regen;
	return hs + comp;
}

static void dec_sequences(struct dec *d, int start, int n)
{
	const u8 *p = d->p;
	if (n < 1)
		bad(d, "no sequences");
	int at = start, b0 = p[at], ns;
	if (b0 < 128) {
		ns = b0;
		at += 1;
	} else if (b0 < 255) {
		if (n < 2)
			bad(d, "bad sequences");
		ns = ((b0 - 128) << 8) + p[at + 1];
		at += 2;
	} else {
		if (n < 3)
			bad(d, "bad sequences");
		ns = p[at + 1] + (p[at + 2] << 8) + 0x7F00;
		at += 3;
	}
	int end = start + n, li = 0;
	if (ns != 0) {
		if (at >= end)
			bad(d, "bad sequences");
		int modes = p[at++];
		if (modes & 3)
			bad(d, "bad sequences");
		fse_t *tabs[3] = { &d->ll, &d->of, &d->ml };
		const int *defs[3] = { LL_DEF, OF_DEF, ML_DEF };
		const int ndefs[3] = { 36, 29, 53 };
		const int nsyms[3] = { 36, 32, 53 };
		const int def_al[3] = { 6, 5, 6 }, max_al[3] = { 9, 8, 9 };
		for (int t = 0; t < 3; t++) {
			switch ((modes >> (6 - 2 * t)) & 3) {
			case 0:
				fse_build(tabs[t], defs[t], ndefs[t], def_al[t]);
				break;
			case 1:
				if (at >= end || p[at] >= nsyms[t])
					bad(d, "bad sequences");
				tabs[t]->log = 0;
				tabs[t]->sym[0] = p[at++];
				tabs[t]->nb[0] = 0;
				tabs[t]->base[0] = 0;
				break;
			case 2: {
				int cnt[256], nc, al;
				int k = fse_desc(p + at, end - at, max_al[t], nsyms[t], cnt, &nc, &al);
				if (k < 0 || !fse_build(tabs[t], cnt, nc, al))
					bad(d, "a bad FSE table");
				at += k;
				break;
			}
			default:
				if (!d->have[t])
					bad(d, "no FSE table to repeat");
			}
			d->have[t] = 1;
		}
		back_bits in;
		if (!back_init(&in, p + at, end - at))
			bad(d, "bad sequences");
		int sl = (int)back_read(&in, d->ll.log);
		int so = (int)back_read(&in, d->of.log);
		int sm = (int)back_read(&in, d->ml.log);
		for (int k = 0; k < ns; k++) {
			int llc = d->ll.sym[sl], ofc = d->of.sym[so], mlc = d->ml.sym[sm];
			if (llc > 35 || mlc > 52 || ofc > 31)
				bad(d, "bad sequences");
			i64 ov = ((i64)1 << ofc) + (i64)back_read(&in, ofc);
			int mlen = ML_BASE[mlc] + (int)back_read(&in, ML_BITS[mlc]);
			int llen = LL_BASE[llc] + (int)back_read(&in, LL_BITS[llc]);
			if (k + 1 < ns) {
				sl = d->ll.base[sl] + (int)back_read(&in, d->ll.nb[sl]);
				sm = d->ml.base[sm] + (int)back_read(&in, d->ml.nb[sm]);
				so = d->of.base[so] + (int)back_read(&in, d->of.nb[so]);
			}
			if (in.off < 0 || li + llen > d->nlit)
				bad(d, "bad sequences");
			for (int j = 0; j < llen; j++)
				emit(d, d->lit[li++]);
			i64 off = apply_offset(d->rep, ov, llen);
			if (off <= 0 || off > d->hn || off > d->window)
				bad(d, "a bad offset");
			int from = d->hn - (int)off;
			for (int j = 0; j < mlen; j++)
				emit(d, d->hist[from + j]);
		}
	}
	while (li < d->nlit)
		emit(d, d->lit[li++]);
}

static void dec_run(struct dec *d, int n)
{
	const u8 *p = d->p;
	if (n < 6)
		bad(d, "too short");
	uint32_t magic = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
	if (magic != ZC_MAGIC)
		bad(d, "not a frame");
	int fhd = p[4], single = (fhd & 0x20) != 0;
	if (fhd & 0x08)
		bad(d, "a reserved bit");
	if (fhd & 3)
		bad(d, "a dictionary");
	int checksum = (fhd & 0x04) != 0, fcs_flag = fhd >> 6;
	int fcs = fcs_flag == 0 ? (single ? 1 : 0) : 1 << fcs_flag;
	int at = 5;
	i64 win = 0, size = -1;
	if (!single) {
		int wd = p[at++];
		i64 wb = (i64)1 << (10 + (wd >> 3));
		win = wb + wb / 8 * (wd & 7);
	}
	if (fcs != 0) {
		if (at + fcs > n)
			bad(d, "too short");
		u64 v = 0;
		for (int k = 0; k < fcs; k++)
			v |= (u64)p[at + k] << (8 * k);
		if (fcs == 2)
			v += 256;
		if (v > (u64)d->max)
			bad(d, "content over the limit");
		size = (i64)v;
		if (single)
			win = size;
		at += fcs;
	}
	if (win > d->max)
		bad(d, "a window over the limit");
	d->window = win > 1 ? win : 1;
	for (;;) {
		if (at + 3 > n)
			bad(d, "too short");
		int h = p[at] | (p[at + 1] << 8) | (p[at + 2] << 16);
		at += 3;
		int last = h & 1, type = (h >> 1) & 3, bs = h >> 3;
		if (type == 3 || bs > MAX_BLOCK)
			bad(d, "a bad block");
		if (type == 0) {
			if (at + bs > n)
				bad(d, "too short");
			for (int k = 0; k < bs; k++)
				emit(d, p[at + k]);
			at += bs;
		} else if (type == 1) {
			if (at + 1 > n)
				bad(d, "too short");
			for (int k = 0; k < bs; k++)
				emit(d, p[at]);
			at += 1;
		} else {
			if (at + bs > n)
				bad(d, "too short");
			int l = dec_literals(d, at, bs);
			dec_sequences(d, at + l, bs - l);
			at += bs;
		}
		if (last)
			break;
	}
	if (checksum) {
		if (at + 4 > n)
			bad(d, "too short");
		u64 sum = zc_xxh64(d->hist, (size_t)d->hn);
		for (int k = 0; k < 4; k++)
			if (p[at + k] != ((sum >> (8 * k)) & 0xff))
				bad(d, "a bad checksum");
		at += 4;
	}
	if (at != n)
		bad(d, "bytes after the frame");
	if (size >= 0 && size != d->hn)
		bad(d, "the wrong size");
}

unsigned char *zc_decode(const unsigned char *frame, int n, int max, int *out_n, const char **why)
{
	struct dec *d = calloc(1, sizeof *d);
	unsigned char *res = NULL;
	if (why)
		*why = "no memory";
	if (!d)
		return NULL;
	d->p = frame;
	d->max = max;
	d->hcap = max < 1024 ? (max > 0 ? max : 1) : 1024;
	d->hist = malloc((size_t)d->hcap);
	d->lit = malloc(MAX_BLOCK);
	d->rep[0] = 1;
	d->rep[1] = 4;
	d->rep[2] = 8;
	if (d->hist && d->lit) {
		if (setjmp(d->jb) == 0) {
			dec_run(d, n);
			res = d->hist;
			*out_n = d->hn;
			d->hist = NULL;
			if (why)
				*why = NULL;
		} else if (why) {
			*why = d->why;
		}
	}
	free(d->hist);
	free(d->lit);
	free(d);
	return res;
}

int zc_frame_len(const unsigned char *b, int n, int max)
{
	if (n < 5)
		return 0;
	uint32_t magic = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
	if (magic != ZC_MAGIC)
		return -1;
	int fhd = b[4];
	/* (no reserved bit, no dictionary; the checksum and the content
	   size in it) */
	if ((fhd & 0x0b) || !(fhd & 0x04) || (!(fhd & 0x20) && (fhd >> 6) == 0))
		return -1;
	int single = (fhd & 0x20) != 0;
	int at = 5 + (single ? 0 : 1);
	switch (fhd >> 6) {
	case 0: at += single ? 1 : 0; break;
	case 1: at += 2; break;
	case 2: at += 4; break;
	default: at += 8;
	}
	for (;;) {
		if (n < at + 3)
			return 0;
		int bh = b[at] | (b[at + 1] << 8) | (b[at + 2] << 16);
		at += 3;
		int size = bh >> 3;
		switch ((bh >> 1) & 3) {
		case 0:
		case 2: at += size; break;
		case 1: at += 1; break;
		default: return -1;
		}
		if (bh & 1)
			break;
		if (at > max + max / 8)
			return -1;
	}
	if (fhd & 0x04)
		at += 4;
	return n < at ? 0 : at;
}

/* ------------------------------------------------------------------ */
/* XXH64 */

#define P1 0x9E3779B185EBCA87ULL
#define P2 0xC2B2AE3D27D4EB4FULL
#define P3 0x165667B19E3779F9ULL
#define P4 0x85EBCA77C2B2AE63ULL
#define P5 0x27D4EB2F165667C5ULL

static u64 rotl(u64 v, int r)
{
	return (v << r) | (v >> (64 - r));
}

static u64 le64(const u8 *b)
{
	u64 v = 0;
	for (int k = 0; k < 8; k++)
		v |= (u64)b[k] << (8 * k);
	return v;
}

static u64 le32(const u8 *b)
{
	return (u64)b[0] | ((u64)b[1] << 8) | ((u64)b[2] << 16) | ((u64)b[3] << 24);
}

static u64 xround(u64 acc, u64 in)
{
	return rotl(acc + in * P2, 31) * P1;
}

static u64 xmerge(u64 acc, u64 v)
{
	return (acc ^ xround(0, v)) * P1 + P4;
}

uint64_t zc_xxh64(const unsigned char *b, size_t len)
{
	const u8 *i = b, *end = b + len;
	u64 h;
	if (len >= 32) {
		u64 v1 = P1 + P2, v2 = P2, v3 = 0, v4 = 0 - P1;
		while (end - i >= 32) {
			v1 = xround(v1, le64(i));
			v2 = xround(v2, le64(i + 8));
			v3 = xround(v3, le64(i + 16));
			v4 = xround(v4, le64(i + 24));
			i += 32;
		}
		h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
		h = xmerge(h, v1);
		h = xmerge(h, v2);
		h = xmerge(h, v3);
		h = xmerge(h, v4);
	} else {
		h = P5;
	}
	h += (u64)len;
	while (end - i >= 8) {
		h = rotl(h ^ xround(0, le64(i)), 27) * P1 + P4;
		i += 8;
	}
	if (end - i >= 4) {
		h = rotl(h ^ (le32(i) * P1), 23) * P2 + P3;
		i += 4;
	}
	while (i < end) {
		h = rotl(h ^ (*i * P5), 11) * P1;
		i++;
	}
	h = (h ^ (h >> 33)) * P2;
	h = (h ^ (h >> 29)) * P3;
	return h ^ (h >> 32);
}
