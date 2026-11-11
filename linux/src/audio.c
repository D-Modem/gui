/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * audio.c - the sound card for voice calls, greetings and messages:
 *           PulseAudio (or PipeWire's pulse server) on GLib's main
 *           loop, so that no thread is needed; 16-bit mono at 8 kHz,
 *           the line's own rate, and the server converts
 *
 * Without libpulse the app builds all the same, with no sound.
 */
#include <string.h>

#include "app.h"

#ifdef HAVE_PULSE

#include <pulse/glib-mainloop.h>
#include <pulse/pulseaudio.h>

/* the speaker's and the microphone's buffers: 40 ms, 20 ms a read */
#define PLAY_MS 40
#define READ_MS 20

static struct {
	App *a;
	pa_glib_mainloop *loop;
	pa_context *ctx;
	pa_stream *play, *rec;
	AudioPull pull;
	AudioPush push;
	gboolean failed;
} au;

static const pa_sample_spec spec = {
	PA_SAMPLE_S16NE, DMC_VOICE_RATE, 1
};

static void writable(pa_stream *st, size_t bytes, void *data)
{
	(void)data;
	while (bytes >= 2) {
		void *p = NULL;
		size_t n = bytes;
		if (pa_stream_begin_write(st, &p, &n) < 0 || !p)
			return;
		int want = (int)(n / 2), got = 0;
		if (au.pull)
			got = au.pull(au.a, p, want);
		if (got < want)
			memset((int16_t *)p + got, 0,
			       (size_t)(want - got) * 2);
		pa_stream_write(st, p, (size_t)want * 2, NULL, 0,
				PA_SEEK_RELATIVE);
		bytes -= (size_t)want * 2;
	}
}

static void readable(pa_stream *st, size_t bytes, void *data)
{
	(void)bytes;
	(void)data;
	const void *p;
	size_t n;
	while (pa_stream_readable_size(st) > 0) {
		if (pa_stream_peek(st, &p, &n) < 0)
			return;
		if (!n)
			break;
		/* (a hole: none of it) */
		if (p && au.push)
			au.push(au.a, p, (int)(n / 2));
		pa_stream_drop(st);
	}
}

static void streams_close(void)
{
	if (au.play) {
		pa_stream_disconnect(au.play);
		pa_stream_unref(au.play);
		au.play = NULL;
	}
	if (au.rec) {
		pa_stream_disconnect(au.rec);
		pa_stream_unref(au.rec);
		au.rec = NULL;
	}
}

static void streams_open(void)
{
	pa_buffer_attr at;
	memset(&at, 0xff, sizeof(at));
	if (au.pull && !au.play) {
		au.play = pa_stream_new(au.ctx, "Voice call", &spec, NULL);
		pa_stream_set_write_callback(au.play, writable, NULL);
		at.tlength = (uint32_t)pa_usec_to_bytes(PLAY_MS * 1000, &spec);
		at.minreq = (uint32_t)pa_usec_to_bytes(READ_MS * 1000, &spec);
		pa_stream_connect_playback(au.play, NULL, &at,
					   PA_STREAM_ADJUST_LATENCY, NULL,
					   NULL);
	}
	memset(&at, 0xff, sizeof(at));
	if (au.push && !au.rec) {
		au.rec = pa_stream_new(au.ctx, "Voice call", &spec, NULL);
		pa_stream_set_read_callback(au.rec, readable, NULL);
		at.fragsize = (uint32_t)pa_usec_to_bytes(READ_MS * 1000, &spec);
		pa_stream_connect_record(au.rec, NULL, &at,
					 PA_STREAM_ADJUST_LATENCY);
	}
}

static gboolean unref_later(gpointer c)
{
	pa_context_unref(c);
	return G_SOURCE_REMOVE;
}

static void context_state(pa_context *c, void *data)
{
	(void)data;
	switch (pa_context_get_state(c)) {
	case PA_CONTEXT_READY:
		streams_open();
		break;
	case PA_CONTEXT_FAILED:
	case PA_CONTEXT_TERMINATED:
		/* (a new context the next time; this one let go outside its
		   own callback) */
		if (au.ctx != c)
			break;
		streams_close();
		au.ctx = NULL;
		pa_context_set_state_callback(c, NULL, NULL);
		g_idle_add(unref_later, c);
		if (au.a && (au.pull || au.push) && !au.failed) {
			au.failed = TRUE;
			window_toast(au.a, "No sound: the sound server is not "
				     "running");
		}
		break;
	default:
		break;
	}
}

gboolean audio_available(void)
{
	return TRUE;
}

gboolean audio_start(App *a, AudioPull pull, AudioPush push)
{
	streams_close();
	au.a = a;
	au.pull = pull;
	au.push = push;
	au.failed = FALSE;
	if (!au.loop)
		au.loop = pa_glib_mainloop_new(NULL);
	if (!au.ctx) {
		pa_proplist *pl = pa_proplist_new();
		pa_proplist_sets(pl, PA_PROP_APPLICATION_ID, APP_ID);
		pa_proplist_sets(pl, PA_PROP_APPLICATION_ICON_NAME, APP_ID);
		pa_proplist_sets(pl, PA_PROP_MEDIA_ROLE, "phone");
		au.ctx = pa_context_new_with_proplist(
			pa_glib_mainloop_get_api(au.loop), "D-Modem", pl);
		pa_proplist_free(pl);
		pa_context_set_state_callback(au.ctx, context_state, NULL);
		if (pa_context_connect(au.ctx, NULL, PA_CONTEXT_NOFLAGS,
				       NULL) < 0) {
			/* (its state callback may have let it go already) */
			if (au.ctx) {
				pa_context_set_state_callback(au.ctx, NULL,
							      NULL);
				pa_context_unref(au.ctx);
				au.ctx = NULL;
			}
			return FALSE;
		}
		return TRUE;
	}
	if (pa_context_get_state(au.ctx) == PA_CONTEXT_READY)
		streams_open();
	return TRUE;
}

void audio_stop(App *a)
{
	(void)a;
	au.pull = NULL;
	au.push = NULL;
	streams_close();
}

#else

gboolean audio_available(void)
{
	return FALSE;
}

gboolean audio_start(App *a, AudioPull pull, AudioPush push)
{
	(void)a;
	(void)pull;
	(void)push;
	return FALSE;
}

void audio_stop(App *a)
{
	(void)a;
}

#endif
