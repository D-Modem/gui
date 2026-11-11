/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * voice.h - what the session's voice calls are made of: G.711 mu-law,
 *           rings of samples, the tones we play (DTMF, the beep)
 */
#ifndef DMC_VOICE_H
#define DMC_VOICE_H

#include <stdint.h>

/* G.711 mu-law, the samples on the DTE in a call (AT+VSM=4) */
unsigned char ulaw_encode(int16_t x);
int16_t ulaw_decode(unsigned char u);
/* and A-law, for WAV files */
int16_t alaw_decode(unsigned char a);

/* a ring of samples that keeps the newest cap of them */
typedef struct dmc_ring {
	int16_t *p;
	int cap, head, n;
} dmc_ring;

void ring_init(dmc_ring *r, int cap);
void ring_free(dmc_ring *r);
void ring_clear(dmc_ring *r);
/* n samples in: the oldest dropped when it is full */
void ring_put(dmc_ring *r, const int16_t *x, int n);
/* up to max out of it: how many */
int ring_get(dmc_ring *r, int16_t *out, int max);
/* the oldest taken out until n are left */
void ring_keep(dmc_ring *r, int n);

/* a tone of one or two frequencies, n samples long */
typedef struct dmc_tone {
	double c1, s1, k1, c2, s2, k2; /* each oscillator's state, step */
	int two, left;
	double amp;
} dmc_tone;

void tone_start(dmc_tone *t, double f1, double f2, int samples,
		double amp);
/* its next sample (0 when it has ended) */
int tone_next(dmc_tone *t);
/* a DTMF digit's two frequencies: 0, or -1 for none */
int dtmf_freqs(char digit, double *lo, double *hi);

#endif
