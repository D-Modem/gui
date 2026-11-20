/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * mmsystem.h - the part of the multimedia API that the app's sound
 *              (audio.cpp, voice.cpp) uses, which the Win32 layer does not
 *              have.  First pass: the functions are in src/stubs.cpp and
 *              answer MMSYSERR_NODRIVER (no sound device).
 */
#ifndef DMODEM_WINPORT_MMSYSTEM_H
#define DMODEM_WINPORT_MMSYSTEM_H
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef UINT MMRESULT;
typedef struct dmodem_hwaveout *HWAVEOUT;
typedef struct dmodem_hwavein *HWAVEIN;

#define MMSYSERR_NOERROR 0
#define MMSYSERR_ERROR 1
#define MMSYSERR_BADDEVICEID 2
#define MMSYSERR_NODRIVER 6
#define WAVE_MAPPER ((UINT)-1)
#define CALLBACK_WINDOW 0x00010000L
#define MM_WOM_DONE 0x3BD
#define MM_WIM_DATA 0x3C0
#define WHDR_DONE 0x00000001
#define WHDR_PREPARED 0x00000002
#define WAVE_FORMAT_PCM 1

#pragma pack(push, 1)
typedef struct tWAVEFORMATEX {
	WORD wFormatTag;
	WORD nChannels;
	DWORD nSamplesPerSec;
	DWORD nAvgBytesPerSec;
	WORD nBlockAlign;
	WORD wBitsPerSample;
	WORD cbSize;
} WAVEFORMATEX, *PWAVEFORMATEX, *LPWAVEFORMATEX;
#pragma pack(pop)
typedef const WAVEFORMATEX *LPCWAVEFORMATEX;

typedef struct wavehdr_tag {
	LPSTR lpData;
	DWORD dwBufferLength;
	DWORD dwBytesRecorded;
	DWORD_PTR dwUser;
	DWORD dwFlags;
	DWORD dwLoops;
	struct wavehdr_tag *lpNext;
	DWORD_PTR reserved;
} WAVEHDR, *PWAVEHDR, *LPWAVEHDR;

MMRESULT WINAPI waveOutOpen(HWAVEOUT *, UINT, LPCWAVEFORMATEX, DWORD_PTR,
			    DWORD_PTR, DWORD);
MMRESULT WINAPI waveOutClose(HWAVEOUT);
MMRESULT WINAPI waveOutPrepareHeader(HWAVEOUT, LPWAVEHDR, UINT);
MMRESULT WINAPI waveOutUnprepareHeader(HWAVEOUT, LPWAVEHDR, UINT);
MMRESULT WINAPI waveOutWrite(HWAVEOUT, LPWAVEHDR, UINT);
MMRESULT WINAPI waveOutReset(HWAVEOUT);
MMRESULT WINAPI waveInOpen(HWAVEIN *, UINT, LPCWAVEFORMATEX, DWORD_PTR,
			   DWORD_PTR, DWORD);
MMRESULT WINAPI waveInClose(HWAVEIN);
MMRESULT WINAPI waveInPrepareHeader(HWAVEIN, LPWAVEHDR, UINT);
MMRESULT WINAPI waveInUnprepareHeader(HWAVEIN, LPWAVEHDR, UINT);
MMRESULT WINAPI waveInAddBuffer(HWAVEIN, LPWAVEHDR, UINT);
MMRESULT WINAPI waveInStart(HWAVEIN);
MMRESULT WINAPI waveInReset(HWAVEIN);

#ifdef __cplusplus
}
#endif
#endif
