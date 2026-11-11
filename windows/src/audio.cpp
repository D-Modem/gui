/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * audio.cpp - the sound card for voice calls, greetings and messages:
 *             waveOut and waveIn, which Windows 95 has, at the line's
 *             own 8 kHz (the wave mapper converts), their buffers
 *             handed back as window messages (no thread of ours)
 *
 * Each buffer that comes back is a chance to run the session too, so
 * that our samples reach the line on time where the timer is coarse
 * (Windows 9x's ticks are 55 ms).
 */
#include <string.h>

#include "app.h"
#include <mmsystem.h>

/* six buffers of 20 ms to the speaker, four from the microphone */
#define FRAME (DMC_VOICE_RATE / 50)
#define OUT_BUFS 6
#define IN_BUFS 4

static HWND win;
static HWAVEOUT wo;
static HWAVEIN wi;
static WAVEHDR oh[OUT_BUFS], ih[IN_BUFS];
static short ob[OUT_BUFS][FRAME], ib[IN_BUFS][FRAME];
static AudioPull pull_fn;
static AudioPush push_fn;
static bool stopping;

/* a speaker's buffer filled from pull (silence where it has none) */
static void fill(WAVEHDR *h)
{
	short *p = (short *)h->lpData;
	int got = pull_fn ? pull_fn(p, FRAME) : 0;
	if (got < 0)
		got = 0;
	if (got < FRAME)
		memset(p + got, 0, (FRAME - got) * sizeof(short));
}

static LRESULT CALLBACK audio_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
	WAVEHDR *h = (WAVEHDR *)lp;
	switch (m) {
	case MM_WOM_DONE:
		/* (a buffer of a device closed since, or queued again
		   already, is not ours to write) */
		if ((HWAVEOUT)wp != wo || !wo || stopping ||
		    !(h->dwFlags & WHDR_DONE))
			return 0;
		fill(h);
		waveOutWrite(wo, h, sizeof(*h));
		link_pump();
		return 0;
	case MM_WIM_DATA:
		if ((HWAVEIN)wp != wi || !wi || stopping ||
		    !(h->dwFlags & WHDR_DONE))
			return 0;
		if (push_fn && h->dwBytesRecorded)
			push_fn((const short *)h->lpData,
				(int)(h->dwBytesRecorded / sizeof(short)));
		waveInAddBuffer(wi, h, sizeof(*h));
		return 0;
	}
	return DefWindowProc(w, m, wp, lp);
}

static bool make_window()
{
	if (win)
		return true;
	WNDCLASS wc;
	memset(&wc, 0, sizeof(wc));
	wc.lpfnWndProc = audio_proc;
	wc.hInstance = app.inst;
	wc.lpszClassName = "DModemAudio";
	RegisterClass(&wc);
	/* (hidden: it only takes the devices' messages) */
	win = CreateWindowEx(0, "DModemAudio", "", WS_POPUP, 0, 0, 0, 0,
			     NULL, NULL, app.inst, NULL);
	return win != NULL;
}

static void format(WAVEFORMATEX *f)
{
	memset(f, 0, sizeof(*f));
	f->wFormatTag = WAVE_FORMAT_PCM;
	f->nChannels = 1;
	f->nSamplesPerSec = DMC_VOICE_RATE;
	f->wBitsPerSample = 16;
	f->nBlockAlign = 2;
	f->nAvgBytesPerSec = DMC_VOICE_RATE * 2;
}

bool audio_start(AudioPull pull, AudioPush push)
{
	audio_stop();
	if (!make_window())
		return false;
	pull_fn = pull;
	push_fn = push;
	WAVEFORMATEX f;
	format(&f);
	bool ok = true;
	if (pull) {
		if (waveOutOpen(&wo, WAVE_MAPPER, &f, (DWORD_PTR)win, 0,
				CALLBACK_WINDOW) != MMSYSERR_NOERROR) {
			wo = NULL;
			ok = false;
		}
		for (int i = 0; wo && i < OUT_BUFS; i++) {
			memset(&oh[i], 0, sizeof(oh[i]));
			oh[i].lpData = (LPSTR)ob[i];
			oh[i].dwBufferLength = sizeof(ob[i]);
			waveOutPrepareHeader(wo, &oh[i], sizeof(oh[i]));
			fill(&oh[i]);
			waveOutWrite(wo, &oh[i], sizeof(oh[i]));
		}
	}
	if (push) {
		if (waveInOpen(&wi, WAVE_MAPPER, &f, (DWORD_PTR)win, 0,
			       CALLBACK_WINDOW) != MMSYSERR_NOERROR) {
			wi = NULL;
			ok = false;
		}
		for (int i = 0; wi && i < IN_BUFS; i++) {
			memset(&ih[i], 0, sizeof(ih[i]));
			ih[i].lpData = (LPSTR)ib[i];
			ih[i].dwBufferLength = sizeof(ib[i]);
			waveInPrepareHeader(wi, &ih[i], sizeof(ih[i]));
			waveInAddBuffer(wi, &ih[i], sizeof(ih[i]));
		}
		if (wi)
			waveInStart(wi);
	}
	return ok;
}

void audio_stop()
{
	stopping = true;
	if (wo) {
		waveOutReset(wo);
		for (int i = 0; i < OUT_BUFS; i++)
			waveOutUnprepareHeader(wo, &oh[i], sizeof(oh[i]));
		waveOutClose(wo);
		wo = NULL;
	}
	if (wi) {
		waveInReset(wi);
		for (int i = 0; i < IN_BUFS; i++)
			waveInUnprepareHeader(wi, &ih[i], sizeof(ih[i]));
		waveInClose(wi);
		wi = NULL;
	}
	pull_fn = NULL;
	push_fn = NULL;
	stopping = false;
}
