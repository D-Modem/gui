/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * msacm.h - the Audio Compression Manager calls that the app's audio.cpp
 *           uses (greetings in MP3 and compressed WAVs).  First pass: the
 *           functions are in src/stubs.cpp and answer MMSYSERR_NODRIVER,
 *           so the app reads only the files that libdmodemctl decodes.
 */
#ifndef DMODEM_WINPORT_MSACM_H
#define DMODEM_WINPORT_MSACM_H
#include <mmsystem.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dmodem_hacmstream *HACMSTREAM;
typedef HACMSTREAM *LPHACMSTREAM;
typedef struct dmodem_hacmdriver *HACMDRIVER;

#define ACM_FORMATSUGGESTF_WFORMATTAG 0x00010000L
#define ACM_FORMATSUGGESTF_NCHANNELS 0x00020000L
#define ACM_FORMATSUGGESTF_NSAMPLESPERSEC 0x00040000L
#define ACM_FORMATSUGGESTF_WBITSPERSAMPLE 0x00080000L
#define ACM_STREAMOPENF_NONREALTIME 0x00000004L
#define ACM_STREAMSIZEF_SOURCE 0x00000000L
#define ACM_STREAMCONVERTF_BLOCKALIGN 0x00000004
#define ACM_STREAMCONVERTF_START 0x00000010
#define ACM_STREAMCONVERTF_END 0x00000020

typedef struct tACMSTREAMHEADER {
	DWORD cbStruct;
	DWORD fdwStatus;
	DWORD_PTR dwUser;
	LPBYTE pbSrc;
	DWORD cbSrcLength;
	DWORD cbSrcLengthUsed;
	DWORD_PTR dwSrcUser;
	LPBYTE pbDst;
	DWORD cbDstLength;
	DWORD cbDstLengthUsed;
	DWORD_PTR dwDstUser;
	DWORD dwReservedDriver[10];
} ACMSTREAMHEADER, *LPACMSTREAMHEADER;

MMRESULT WINAPI acmFormatSuggest(HACMDRIVER, LPWAVEFORMATEX, LPWAVEFORMATEX,
				 DWORD, DWORD);
MMRESULT WINAPI acmStreamOpen(LPHACMSTREAM, HACMDRIVER, LPWAVEFORMATEX,
			      LPWAVEFORMATEX, LPVOID, DWORD_PTR, DWORD_PTR,
			      DWORD);
MMRESULT WINAPI acmStreamClose(HACMSTREAM, DWORD);
MMRESULT WINAPI acmStreamSize(HACMSTREAM, DWORD, LPDWORD, DWORD);
MMRESULT WINAPI acmStreamPrepareHeader(HACMSTREAM, LPACMSTREAMHEADER, DWORD);
MMRESULT WINAPI acmStreamUnprepareHeader(HACMSTREAM, LPACMSTREAMHEADER,
					 DWORD);
MMRESULT WINAPI acmStreamConvert(HACMSTREAM, LPACMSTREAMHEADER, DWORD);

#ifdef __cplusplus
}
#endif
#endif
