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

static unsigned get16(const char *p)
{
	return (unsigned char)p[0] | ((unsigned)(unsigned char)p[1] << 8);
}

static unsigned long get32(const char *p)
{
	return get16(p) | ((unsigned long)get16(p + 2) << 16);
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

int dmc_wav_decode(const char *data, int n, int16_t **pcm)
{
	unsigned format = 0, channels = 0, bits = 0;
	unsigned long rate = 0;
	const char *samples = NULL;
	long bytes = 0, frames, out_n, i;
	int have_fmt = 0, width;
	const char *p, *end;
	int16_t *out;

	*pcm = NULL;
	if (!data || n < 12 || memcmp(data, "RIFF", 4) ||
	    memcmp(data + 8, "WAVE", 4))
		return -1;
	p = data + 12;
	end = data + n;
	while (end - p >= 8) {
		unsigned long len = get32(p + 4);
		const char *body = p + 8;
		long left = (long)(end - body);
		if (!memcmp(p, "fmt ", 4) && len >= 16 && left >= 16) {
			format = get16(body);
			channels = get16(body + 2);
			rate = get32(body + 4);
			bits = get16(body + 14);
			/* WAVE_FORMAT_EXTENSIBLE: its subformat's code */
			if (format == 0xfffe && len >= 26 && left >= 26)
				format = get16(body + 24);
			have_fmt = 1;
		} else if (!memcmp(p, "data", 4)) {
			samples = body;
			bytes = len > (unsigned long)left ? left : (long)len;
			break;
		}
		if (len > (unsigned long)left)
			break;
		p = body + len + (len & 1);
	}
	if (!have_fmt || !samples || !channels || channels > 8 ||
	    rate < 1000 || rate > 192000)
		return -1;
	if (format == 1 && (bits == 8 || bits == 16))
		width = (int)bits / 8;
	else if ((format == 6 || format == 7) && bits == 8)
		width = 1;
	else
		return -1;
	frames = bytes / (long)(width * (int)channels);
	out_n = (long)((double)frames * DMC_VOICE_RATE / (double)rate);
	out = malloc((size_t)(out_n > 0 ? out_n : 1) * sizeof(int16_t));
	if (!out)
		return -1;
	for (i = 0; i < out_n; i++) {
		/* (linear between the two frames around it, each the mean
		   of its channels) */
		double at = (double)i * (double)rate / DMC_VOICE_RATE;
		long f = (long)at;
		double frac = at - (double)f, y = 0;
		int k, c;
		for (k = 0; k < 2; k++) {
			long ff = f + k < frames ? f + k : frames - 1;
			double sum = 0;
			for (c = 0; c < (int)channels; c++) {
				const char *q = samples +
					(ff * (long)channels + c) * width;
				int v;
				if (format == 7)
					v = ulaw_decode((unsigned char)*q);
				else if (format == 6)
					v = alaw_decode((unsigned char)*q);
				else if (width == 1)
					v = ((int)(unsigned char)*q - 128) * 256;
				else
					v = (int16_t)get16(q);
				sum += v;
			}
			sum /= channels;
			y += k ? frac * sum : (1 - frac) * sum;
		}
		out[i] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
	}
	*pcm = out;
	return (int)out_n;
}

void dmc_free(void *p)
{
	free(p);
}
