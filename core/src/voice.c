/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * voice.c - G.711, rings of samples, tones, and WAV files (dmodemctl.h)
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "dmodemctl.h"
#include "voice.h"

/*
 * G.711
 */

unsigned char ulaw_encode(int16_t x)
{
	int v = x, sign = 0, exp, mant;
	if (v < 0) {
		v = -v;
		sign = 0x80;
	}
	v += 0x84;
	if (v > 0x7fff)
		v = 0x7fff;
	for (exp = 7; exp > 0 && !(v & (0x4000 >> (7 - exp))); exp--)
		;
	mant = (v >> (exp + 3)) & 0x0f;
	return (unsigned char)~(sign | (exp << 4) | mant);
}

int16_t ulaw_decode(unsigned char u)
{
	int v, exp, mant;
	u = (unsigned char)~u;
	exp = (u >> 4) & 7;
	mant = u & 0x0f;
	v = ((mant << 3) + 0x84) << exp;
	v -= 0x84;
	return (int16_t)((u & 0x80) ? -v : v);
}

int16_t alaw_decode(unsigned char a)
{
	int v, exp, mant;
	a ^= 0x55;
	exp = (a >> 4) & 7;
	mant = a & 0x0f;
	v = exp ? ((mant << 4) + 0x108) << (exp - 1) : (mant << 4) + 8;
	return (int16_t)((a & 0x80) ? v : -v);
}

/*
 * Rings
 */

void ring_init(dmc_ring *r, int cap)
{
	memset(r, 0, sizeof(*r));
	r->p = calloc((size_t)cap, sizeof(int16_t));
	if (!r->p)
		abort();
	r->cap = cap;
}

void ring_free(dmc_ring *r)
{
	free(r->p);
	memset(r, 0, sizeof(*r));
}

void ring_clear(dmc_ring *r)
{
	r->head = r->n = 0;
}

void ring_put(dmc_ring *r, const int16_t *x, int n)
{
	int i;
	if (!r->cap)
		return;
	for (i = 0; i < n; i++) {
		int at = (r->head + r->n) % r->cap;
		r->p[at] = x ? x[i] : 0;
		if (r->n < r->cap)
			r->n++;
		else
			r->head = (r->head + 1) % r->cap;
	}
}

int ring_get(dmc_ring *r, int16_t *out, int max)
{
	int i, n = r->n < max ? r->n : max;
	for (i = 0; i < n; i++) {
		out[i] = r->p[r->head];
		r->head = (r->head + 1) % r->cap;
	}
	r->n -= n;
	return n;
}

void ring_keep(dmc_ring *r, int n)
{
	if (r->n <= n)
		return;
	r->head = (r->head + r->n - n) % r->cap;
	r->n = n;
}

/*
 * Tones
 */

#define PI 3.14159265358979323846

void tone_start(dmc_tone *t, double f1, double f2, int samples,
		double amp)
{
	memset(t, 0, sizeof(*t));
	t->k1 = 2 * PI * f1 / DMC_VOICE_RATE;
	t->k2 = 2 * PI * f2 / DMC_VOICE_RATE;
	t->c1 = t->c2 = 1;
	t->two = f2 > 0;
	t->left = samples;
	t->amp = amp;
}

/* (each oscillator a unit vector turned by its step each sample) */
static double turn(double *c, double *s, double k)
{
	double c0 = *c, s0 = *s, ck = cos(k), sk = sin(k), m;
	*c = c0 * ck - s0 * sk;
	*s = c0 * sk + s0 * ck;
	/* (kept on the circle) */
	m = sqrt(*c * *c + *s * *s);
	if (m > 0) {
		*c /= m;
		*s /= m;
	}
	return s0;
}

int tone_next(dmc_tone *t)
{
	double y;
	if (t->left <= 0)
		return 0;
	t->left--;
	y = turn(&t->c1, &t->s1, t->k1);
	if (t->two)
		y = (y + turn(&t->c2, &t->s2, t->k2)) / 2;
	return (int)(y * t->amp);
}

int dtmf_freqs(char digit, double *lo, double *hi)
{
	static const char keys[] = "123A456B789C*0#D";
	static const double row[] = { 697, 770, 852, 941 };
	static const double col[] = { 1209, 1336, 1477, 1633 };
	const char *k;
	char d = digit;
	if (d >= 'a' && d <= 'd')
		d = (char)(d - 'a' + 'A');
	k = d ? strchr(keys, d) : NULL;
	if (!k)
		return -1;
	*lo = row[(k - keys) / 4];
	*hi = col[(k - keys) % 4];
	return 0;
}

/*
 * WAV files
 */

static void put16(char *p, unsigned v)
{
	p[0] = (char)(v & 0xff);
	p[1] = (char)((v >> 8) & 0xff);
}

static void put32(char *p, unsigned long v)
{
	put16(p, (unsigned)(v & 0xffff));
	put16(p + 2, (unsigned)((v >> 16) & 0xffff));
}

int dmc_wav_header(char *buf, int n)
{
	unsigned long bytes = n > 0 ? (unsigned long)n * 2 : 0;
	memcpy(buf, "RIFF", 4);
	put32(buf + 4, 36 + bytes);
	memcpy(buf + 8, "WAVEfmt ", 8);
	put32(buf + 16, 16);
	put16(buf + 20, 1);                     /* PCM */
	put16(buf + 22, 1);                     /* mono */
	put32(buf + 24, DMC_VOICE_RATE);
	put32(buf + 28, DMC_VOICE_RATE * 2);
	put16(buf + 32, 2);
	put16(buf + 34, 16);
	memcpy(buf + 36, "data", 4);
	put32(buf + 40, bytes);
	return 44;
}

/*
 * Sound files read: WAV (PCM of 8 to 32 bits, floats, A-law, mu-law,
 * IMA and Microsoft ADPCM), AIFF and AIFF-C (PCM, floats, A-law,
 * mu-law), Sun/NeXT .au; any rate and channels, made 8 kHz mono
 */

/* what a file's samples are */
enum { E_PCM, E_FLOAT, E_ALAW, E_ULAW, E_IMA, E_MSADPCM };

struct sound {
	int enc, bits, big, channels, block;
	double rate;
	const unsigned char *p;
	long bytes;
	/* (MS ADPCM's coefficient pairs, from its fmt chunk) */
	int ncoef;
	int coef[64][2];
};

static unsigned long get32be(const unsigned char *p)
{
	return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
	       ((unsigned long)p[2] << 8) | p[3];
}

static unsigned ug16(const unsigned char *p)
{
	return p[0] | ((unsigned)p[1] << 8);
}

static unsigned long ug32(const unsigned char *p)
{
	return ug16(p) | ((unsigned long)ug16(p + 2) << 16);
}

/* an 80-bit IEEE extended (AIFF's rate) as a double */
static double extended(const unsigned char *p)
{
	int e = ((p[0] & 0x7f) << 8 | p[1]) - 16383 - 63;
	double m = (double)get32be(p + 2) * 4294967296.0 +
		   (double)get32be(p + 6);
	double v = ldexp(m, e);
	return (p[0] & 0x80) ? -v : v;
}

/* a RIFF WAVE's: 0, or -1 if it isn't one this reads */
static int parse_wav(const unsigned char *d, long n, struct sound *f)
{
	const unsigned char *p = d + 12, *end = d + n;
	int fmt = 0, tag = 0;
	while (end - p >= 8) {
		unsigned long len = ug32(p + 4);
		const unsigned char *body = p + 8;
		long left = (long)(end - body);
		if (!memcmp(p, "fmt ", 4) && len >= 16 && left >= 16) {
			tag = (int)ug16(body);
			f->channels = (int)ug16(body + 2);
			f->rate = ug32(body + 4);
			f->block = (int)ug16(body + 12);
			f->bits = (int)ug16(body + 14);
			/* WAVE_FORMAT_EXTENSIBLE: its subformat's code */
			if (tag == 0xfffe && len >= 26 && left >= 26)
				tag = (int)ug16(body + 24);
			if (tag == 2 && len >= 22 && left >= 22) {
				int i, k = (int)ug16(body + 20);
				if (k > 64 || (long)(22 + 4 * k) > left)
					return -1;
				f->ncoef = k;
				for (i = 0; i < k; i++) {
					f->coef[i][0] = (int16_t)ug16(body + 22 + 4 * i);
					f->coef[i][1] = (int16_t)ug16(body + 24 + 4 * i);
				}
			}
			fmt = 1;
		} else if (!memcmp(p, "data", 4)) {
			f->p = body;
			f->bytes = len > (unsigned long)left ? left : (long)len;
			break;
		}
		if (len > (unsigned long)left)
			break;
		p = body + len + (len & 1);
	}
	if (!fmt || !f->p)
		return -1;
	switch (tag) {
	case 1: f->enc = E_PCM; break;
	case 3: f->enc = E_FLOAT; break;
	case 6: f->enc = E_ALAW; break;
	case 7: f->enc = E_ULAW; break;
	case 0x11: f->enc = E_IMA; break;
	case 2: f->enc = E_MSADPCM; break;
	default: return -1;
	}
	return 0;
}

/* an AIFF or AIFF-C's */
static int parse_aiff(const unsigned char *d, long n, struct sound *f)
{
	const unsigned char *p = d + 12, *end = d + n;
	int comm = 0, aifc = !memcmp(d + 8, "AIFC", 4);
	char comp[4] = { 'N', 'O', 'N', 'E' };
	while (end - p >= 8) {
		unsigned long len = get32be(p + 4);
		const unsigned char *body = p + 8;
		long left = (long)(end - body);
		if (!memcmp(p, "COMM", 4) && len >= 18 && left >= 18) {
			f->channels = (int)(body[0] << 8 | body[1]);
			f->bits = (int)(body[6] << 8 | body[7]);
			f->rate = extended(body + 8);
			if (aifc && len >= 22 && left >= 22)
				memcpy(comp, body + 18, 4);
			comm = 1;
		} else if (!memcmp(p, "SSND", 4) && len >= 8 && left >= 8) {
			unsigned long off = get32be(body);
			long l = len > (unsigned long)left ? left : (long)len;
			if ((long)off > l - 8)
				return -1;
			f->p = body + 8 + off;
			f->bytes = l - 8 - (long)off;
		}
		if (len > (unsigned long)left)
			break;
		p = body + len + (len & 1);
	}
	if (!comm || !f->p)
		return -1;
	f->big = 1;
	if (!memcmp(comp, "NONE", 4) || !memcmp(comp, "twos", 4)) {
		f->enc = E_PCM;
	} else if (!memcmp(comp, "sowt", 4)) {
		f->enc = E_PCM;
		f->big = 0;
	} else if (!memcmp(comp, "fl32", 4) || !memcmp(comp, "FL32", 4)) {
		f->enc = E_FLOAT;
		f->bits = 32;
	} else if (!memcmp(comp, "fl64", 4) || !memcmp(comp, "FL64", 4)) {
		f->enc = E_FLOAT;
		f->bits = 64;
	} else if (!memcmp(comp, "ulaw", 4) || !memcmp(comp, "ULAW", 4)) {
		f->enc = E_ULAW;
		f->bits = 8;
	} else if (!memcmp(comp, "alaw", 4) || !memcmp(comp, "ALAW", 4)) {
		f->enc = E_ALAW;
		f->bits = 8;
	} else {
		return -1;
	}
	/* (AIFF's 8-bit samples are signed) */
	if (f->enc == E_PCM && f->bits == 8)
		f->big = 2;
	return 0;
}

/* a Sun/NeXT .au's */
static int parse_au(const unsigned char *d, long n, struct sound *f)
{
	unsigned long off, len, enc;
	if (n < 24)
		return -1;
	off = get32be(d + 4);
	len = get32be(d + 8);
	enc = get32be(d + 12);
	f->rate = get32be(d + 16);
	f->channels = (int)get32be(d + 20);
	if (off < 24 || (long)off > n)
		return -1;
	f->p = d + off;
	f->bytes = n - (long)off;
	if (len != 0xffffffffUL && (long)len < f->bytes)
		f->bytes = (long)len;
	f->big = 1;
	switch (enc) {
	case 1: f->enc = E_ULAW; f->bits = 8; break;
	case 2: f->enc = E_PCM; f->bits = 8; f->big = 2; break;
	case 3: f->enc = E_PCM; f->bits = 16; break;
	case 4: f->enc = E_PCM; f->bits = 24; break;
	case 5: f->enc = E_PCM; f->bits = 32; break;
	case 6: f->enc = E_FLOAT; f->bits = 32; break;
	case 7: f->enc = E_FLOAT; f->bits = 64; break;
	case 27: f->enc = E_ALAW; f->bits = 8; break;
	default: return -1;
	}
	return 0;
}

/* a sample of width w bytes at q as 16 bits (big: 1 big-endian, 2
   big-endian with 8-bit samples signed) */
static int pcm_at(const unsigned char *q, int w, int big)
{
	unsigned long v = 0;
	int i;
	if (w == 1)
		return big == 2 ? (int)(signed char)q[0] * 256 :
				  ((int)q[0] - 128) * 256;
	for (i = 0; i < w; i++)
		v |= (unsigned long)q[big ? i : w - 1 - i] << (8 * (w - 1 - i));
	/* (the top two bytes: the sample's sign is the top bit) */
	v >>= 8 * (w - 2);
	return (int16_t)(v & 0xffff);
}

static int float_at(const unsigned char *q, int w, int big)
{
	unsigned char b[8];
	double x;
	int i;
	for (i = 0; i < w; i++)
		b[i] = q[big ? w - 1 - i : i];
	/* (b is little-endian now, as the machines this runs on are) */
	if (w == 4) {
		float y;
		memcpy(&y, b, 4);
		x = y;
	} else {
		memcpy(&x, b, 8);
	}
	if (x != x)
		return 0;
	x *= 32767.0;
	return x > 32767 ? 32767 : x < -32768 ? -32768 : (int)x;
}

static const int ima_steps[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34,
	37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
	157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494,
	544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
	1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
	4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
	12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086,
	29794, 32767
};
static const int ima_index[16] = { -1, -1, -1, -1, 2, 4, 6, 8,
				   -1, -1, -1, -1, 2, 4, 6, 8 };

static int ima_nibble(int *pred, int *idx, int nib)
{
	int step = ima_steps[*idx], diff = step >> 3;
	if (nib & 4) diff += step;
	if (nib & 2) diff += step >> 1;
	if (nib & 1) diff += step >> 2;
	*pred += (nib & 8) ? -diff : diff;
	if (*pred > 32767) *pred = 32767;
	if (*pred < -32768) *pred = -32768;
	*idx += ima_index[nib];
	if (*idx < 0) *idx = 0;
	if (*idx > 88) *idx = 88;
	return *pred;
}

/* one channel's sample of frame i: the files' samples mixed to mono
   into out (frames of them): how many frames */
static long decode_mono(const struct sound *f, int16_t *out, long cap)
{
	int c, ch = f->channels;
	long fr = 0;
	if (f->enc == E_IMA) {
		/* blocks: per channel a header (sample, index), then 4-byte
		   groups of 8 nibbles per channel in turn */
		const unsigned char *b = f->p, *end = f->p + f->bytes;
		int per = f->block > 4 * ch ? (f->block - 4 * ch) * 2 / ch + 1 : 0;
		if (per <= 0 || per > 65536)
			return -1;
		while (end - b >= f->block && fr < cap) {
			int pred[8], idx[8], i;
			int16_t *o = out + fr;
			long n = per < cap - fr ? per : cap - fr;
			for (i = 0; i < n; i++)
				o[i] = 0;
			for (c = 0; c < ch; c++) {
				const unsigned char *h = b + 4 * c;
				pred[c] = (int16_t)ug16(h);
				idx[c] = h[2] > 88 ? 88 : h[2];
			}
			for (c = 0; c < ch; c++) {
				long k = 0;
				const unsigned char *q = b + 4 * ch + 4 * c;
				if (k < n)
					o[k] = (int16_t)(o[k] + pred[c] / ch);
				k = 1;
				while (k < per && q + 4 <= b + f->block) {
					int j;
					for (j = 0; j < 8 && k < per; j++, k++) {
						int nib = (q[j / 2] >> ((j & 1) * 4)) & 15;
						int v = ima_nibble(&pred[c], &idx[c], nib);
						if (k < n)
							o[k] = (int16_t)(o[k] + v / ch);
					}
					q += 4 * ch;
				}
			}
			fr += n;
			b += f->block;
		}
		return fr;
	}
	if (f->enc == E_MSADPCM) {
		static const int adapt[16] = { 230, 230, 230, 230, 307, 409,
					       512, 614, 768, 614, 512, 409,
					       307, 230, 230, 230 };
		const unsigned char *b = f->p, *end = f->p + f->bytes;
		int per;
		if (ch > 2 || !f->ncoef || f->block < 7 * ch)
			return -1;
		per = (f->block - 7 * ch) * 2 / ch + 2;
		while (end - b >= f->block && fr < cap) {
			int pred[2], delta[2], s1[2], s2[2];
			const unsigned char *q = b;
			long n = per < cap - fr ? per : cap - fr, k;
			int16_t *o = out + fr;
			for (c = 0; c < ch; c++)
				pred[c] = q[c] < f->ncoef ? q[c] : 0;
			q += ch;
			for (c = 0; c < ch; c++, q += 2)
				delta[c] = (int16_t)ug16(q);
			for (c = 0; c < ch; c++, q += 2)
				s1[c] = (int16_t)ug16(q);
			for (c = 0; c < ch; c++, q += 2)
				s2[c] = (int16_t)ug16(q);
			/* (the header's two samples: the older first) */
			if (n > 0) {
				long v = 0;
				for (c = 0; c < ch; c++) v += s2[c];
				o[0] = (int16_t)(v / ch);
			}
			if (n > 1) {
				long v = 0;
				for (c = 0; c < ch; c++) v += s1[c];
				o[1] = (int16_t)(v / ch);
			}
			for (k = 2; k < per; k++) {
				long v = 0;
				for (c = 0; c < ch; c++) {
					long nib_i = (k - 2) * ch + c;
					int nib = (q[nib_i / 2] >> ((nib_i & 1) ? 0 : 4)) & 15;
					int sn = nib & 8 ? nib - 16 : nib;
					long long p = ((long long)s1[c] * f->coef[pred[c]][0] +
						       (long long)s2[c] * f->coef[pred[c]][1]) / 256;
					p += (long long)sn * delta[c];
					if (p > 32767) p = 32767;
					if (p < -32768) p = -32768;
					s2[c] = s1[c];
					s1[c] = (int)p;
					delta[c] = (int)((long)delta[c] * adapt[nib] / 256);
					/* (a broken file's grows without end) */
					if (delta[c] > 0x100000) delta[c] = 0x100000;
					if (delta[c] < 16) delta[c] = 16;
					v += (long)p;
				}
				if (k < n)
					o[k] = (int16_t)(v / ch);
			}
			fr += n;
			b += f->block;
		}
		return fr;
	}
	{
		int w = f->bits / 8;
		long frames, i;
		if (w < 1 || w > 8 || (f->enc == E_PCM && w > 4) ||
		    (f->enc == E_FLOAT && w != 4 && w != 8) ||
		    ((f->enc == E_ALAW || f->enc == E_ULAW) && w != 1))
			return -1;
		frames = f->bytes / ((long)w * ch);
		if (frames > cap)
			frames = cap;
		for (i = 0; i < frames; i++) {
			long sum = 0;
			for (c = 0; c < ch; c++) {
				const unsigned char *q = f->p + (i * ch + c) * w;
				switch (f->enc) {
				case E_ULAW: sum += ulaw_decode(*q); break;
				case E_ALAW: sum += alaw_decode(*q); break;
				case E_FLOAT: sum += float_at(q, w, f->big); break;
				default: sum += pcm_at(q, w, f->big); break;
				}
			}
			out[i] = (int16_t)(sum / ch);
		}
		return frames;
	}
}

/* the most frames f could hold */
static long most_frames(const struct sound *f)
{
	if (f->enc == E_IMA || f->enc == E_MSADPCM) {
		if (f->block <= 0)
			return 0;
		return (f->bytes / f->block + 1) * ((long)f->block * 2 + 2);
	}
	if (f->bits < 8)
		return 0;
	return f->bytes / ((long)(f->bits / 8) * f->channels);
}

int dmc_audio_decode(const char *data, int n, int16_t **pcm)
{
	const unsigned char *d = (const unsigned char *)data;
	struct sound f;
	int16_t *mono, *out;
	long frames, cap, out_n, i;
	int r = -1;

	*pcm = NULL;
	if (!data || n < 12)
		return -1;
	memset(&f, 0, sizeof(f));
	if (!memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WAVE", 4))
		r = parse_wav(d, n, &f);
	else if (!memcmp(d, "FORM", 4) &&
		 (!memcmp(d + 8, "AIFF", 4) || !memcmp(d + 8, "AIFC", 4)))
		r = parse_aiff(d, n, &f);
	else if (!memcmp(d, ".snd", 4))
		r = parse_au(d, n, &f);
	if (r || f.channels < 1 || f.channels > 8 || !(f.rate >= 1000) ||
	    f.rate > 384000)
		return -1;
	cap = most_frames(&f);
	if (cap < 0 || cap > 100000000L)
		return -1;
	mono = malloc((size_t)(cap > 0 ? cap : 1) * sizeof(int16_t));
	if (!mono)
		return -1;
	frames = decode_mono(&f, mono, cap);
	if (frames < 0) {
		free(mono);
		return -1;
	}
	out_n = (long)((double)frames * DMC_VOICE_RATE / f.rate);
	out = malloc((size_t)(out_n > 0 ? out_n : 1) * sizeof(int16_t));
	if (!out) {
		free(mono);
		return -1;
	}
	for (i = 0; i < out_n; i++) {
		/* (linear between the two frames around it) */
		double at = (double)i * f.rate / DMC_VOICE_RATE;
		long k = (long)at;
		double frac = at - (double)k;
		long k1 = k + 1 < frames ? k + 1 : frames - 1;
		double y = (1 - frac) * mono[k] + frac * mono[k1];
		out[i] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
	}
	free(mono);
	*pcm = out;
	return (int)out_n;
}

int dmc_wav_decode(const char *data, int n, int16_t **pcm)
{
	return dmc_audio_decode(data, n, pcm);
}

void dmc_free(void *p)
{
	free(p);
}
