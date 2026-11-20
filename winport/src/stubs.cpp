/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * stubs.cpp - the Win32 calls of the app that the Win32 layer of
 *             clang-retro MFC does not have yet (include/).  First pass:
 *             the sound, ACM, serial and pipe calls fail, so the app
 *             operates through TCP (a dmodemd that already runs) and
 *             SSH, Telnet and rlogin.  Remove each one when the layer
 *             has it.
 */
#include <windows.h>
#include <mmsystem.h>
#include <msacm.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

extern "C" {

/* sound: no device */
MMRESULT WINAPI waveOutOpen(HWAVEOUT *, UINT, LPCWAVEFORMATEX, DWORD_PTR,
			    DWORD_PTR, DWORD) { return MMSYSERR_NODRIVER; }
MMRESULT WINAPI waveOutClose(HWAVEOUT) { return MMSYSERR_NODRIVER; }
MMRESULT WINAPI waveOutPrepareHeader(HWAVEOUT, LPWAVEHDR, UINT) { return MMSYSERR_NODRIVER; }
MMRESULT WINAPI waveOutUnprepareHeader(HWAVEOUT, LPWAVEHDR, UINT) { return MMSYSERR_NODRIVER; }
MMRESULT WINAPI waveOutWrite(HWAVEOUT, LPWAVEHDR, UINT) { return MMSYSERR_NODRIVER; }
MMRESULT WINAPI waveOutReset(HWAVEOUT) { return MMSYSERR_NODRIVER; }
MMRESULT WINAPI waveInOpen(HWAVEIN *, UINT, LPCWAVEFORMATEX, DWORD_PTR,
			   DWORD_PTR, DWORD) { return MMSYSERR_NODRIVER; }
MMRESULT WINAPI waveInClose(HWAVEIN) { return MMSYSERR_NODRIVER; }
MMRESULT WINAPI waveInPrepareHeader(HWAVEIN, LPWAVEHDR, UINT) { return MMSYSERR_NODRIVER; }
MMRESULT WINAPI waveInUnprepareHeader(HWAVEIN, LPWAVEHDR, UINT) { return MMSYSERR_NODRIVER; }
MMRESULT WINAPI waveInAddBuffer(HWAVEIN, LPWAVEHDR, UINT) { return MMSYSERR_NODRIVER; }
MMRESULT WINAPI waveInStart(HWAVEIN) { return MMSYSERR_NODRIVER; }
MMRESULT WINAPI waveInReset(HWAVEIN) { return MMSYSERR_NODRIVER; }

/* ACM: no codecs */
MMRESULT WINAPI acmFormatSuggest(HACMDRIVER, LPWAVEFORMATEX, LPWAVEFORMATEX,
				 DWORD, DWORD) { return MMSYSERR_NODRIVER; }
MMRESULT WINAPI acmStreamOpen(LPHACMSTREAM, HACMDRIVER, LPWAVEFORMATEX,
			      LPWAVEFORMATEX, LPVOID, DWORD_PTR, DWORD_PTR,
			      DWORD) { return MMSYSERR_NODRIVER; }
MMRESULT WINAPI acmStreamClose(HACMSTREAM, DWORD) { return MMSYSERR_NODRIVER; }
MMRESULT WINAPI acmStreamSize(HACMSTREAM, DWORD, LPDWORD, DWORD) { return MMSYSERR_NODRIVER; }
MMRESULT WINAPI acmStreamPrepareHeader(HACMSTREAM, LPACMSTREAMHEADER, DWORD) { return MMSYSERR_NODRIVER; }
MMRESULT WINAPI acmStreamUnprepareHeader(HACMSTREAM, LPACMSTREAMHEADER, DWORD) { return MMSYSERR_NODRIVER; }
MMRESULT WINAPI acmStreamConvert(HACMSTREAM, LPACMSTREAMHEADER, DWORD) { return MMSYSERR_NODRIVER; }

#ifndef DMODEM_WINPORT_LAYER_KERNEL
/* serial ports: none */
BOOL WINAPI GetCommState(HANDLE, LPDCB)
{
	SetLastError(ERROR_NOT_SUPPORTED);
	return FALSE;
}
BOOL WINAPI SetCommState(HANDLE, LPDCB)
{
	SetLastError(ERROR_NOT_SUPPORTED);
	return FALSE;
}
BOOL WINAPI SetCommTimeouts(HANDLE, LPCOMMTIMEOUTS)
{
	SetLastError(ERROR_NOT_SUPPORTED);
	return FALSE;
}

/* anonymous pipes: none (no dmodemd started by the app) */
BOOL WINAPI CreatePipe(PHANDLE, PHANDLE, LPSECURITY_ATTRIBUTES, DWORD)
{
	SetLastError(ERROR_NOT_SUPPORTED);
	return FALSE;
}
BOOL WINAPI PeekNamedPipe(HANDLE, LPVOID, DWORD, LPDWORD, LPDWORD, LPDWORD)
{
	SetLastError(ERROR_NOT_SUPPORTED);
	return FALSE;
}

/* the environment block of Windows (NAME=VALUE\0 ... \0) from environ */
LPSTR WINAPI dmodem_GetEnvironmentStringsA(void)
{
	size_t n = 1;
	for (char **e = environ; *e; e++)
		n += strlen(*e) + 1;
	char *block = (char *)malloc(n), *p = block;
	if (!block)
		return NULL;
	for (char **e = environ; *e; e++) {
		size_t l = strlen(*e) + 1;
		memcpy(p, *e, l);
		p += l;
	}
	*p = '\0';
	return block;
}
BOOL WINAPI dmodem_FreeEnvironmentStringsA(LPSTR block)
{
	free(block);
	return TRUE;
}

BOOL WINAPI GetUserNameA(LPSTR name, LPDWORD n)
{
	const char *u = getenv("USER");
	if (!u)
		u = getlogin();
	if (!u || strlen(u) + 1 > *n) {
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	strcpy(name, u);
	*n = (DWORD)strlen(u) + 1;
	return TRUE;
}

/* (no speaker: the terminal's bell is MessageBeep) */
BOOL WINAPI Beep(DWORD, DWORD)
{
	MessageBeep(MB_OK);
	return TRUE;
}

#endif

}
