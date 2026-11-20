/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * windows.h - the Win32 layer of clang-retro MFC (port/), and the calls of
 *             the app that it does not have yet: serial ports, anonymous
 *             pipes, the environment block, the user name, Beep and
 *             the declaration of WinMain.
 *             First pass: src/stubs.cpp has them.  The serial and pipe
 *             calls fail, so the app reaches a modem through TCP only.
 */
#ifndef DMODEM_WINPORT_WINDOWS_H
#define DMODEM_WINPORT_WINDOWS_H
#include_next <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* (the SDK's winbase.h declares it, with C linkage; the layer's crt0
   calls it) */
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int);

/* serial ports */
#define CBR_115200 115200
#define NOPARITY 0
#define ONESTOPBIT 0
#define DTR_CONTROL_ENABLE 0x01
#define RTS_CONTROL_HANDSHAKE 0x02

typedef struct _DCB {
	DWORD DCBlength;
	DWORD BaudRate;
	DWORD fBinary : 1;
	DWORD fParity : 1;
	DWORD fOutxCtsFlow : 1;
	DWORD fOutxDsrFlow : 1;
	DWORD fDtrControl : 2;
	DWORD fDsrSensitivity : 1;
	DWORD fTXContinueOnXoff : 1;
	DWORD fOutX : 1;
	DWORD fInX : 1;
	DWORD fErrorChar : 1;
	DWORD fNull : 1;
	DWORD fRtsControl : 2;
	DWORD fAbortOnError : 1;
	DWORD fDummy2 : 17;
	WORD wReserved;
	WORD XonLim;
	WORD XoffLim;
	BYTE ByteSize;
	BYTE Parity;
	BYTE StopBits;
	char XonChar;
	char XoffChar;
	char ErrorChar;
	char EofChar;
	char EvtChar;
	WORD wReserved1;
} DCB, *LPDCB;

typedef struct _COMMTIMEOUTS {
	DWORD ReadIntervalTimeout;
	DWORD ReadTotalTimeoutMultiplier;
	DWORD ReadTotalTimeoutConstant;
	DWORD WriteTotalTimeoutMultiplier;
	DWORD WriteTotalTimeoutConstant;
} COMMTIMEOUTS, *LPCOMMTIMEOUTS;

BOOL WINAPI GetCommState(HANDLE, LPDCB);
BOOL WINAPI SetCommState(HANDLE, LPDCB);
BOOL WINAPI SetCommTimeouts(HANDLE, LPCOMMTIMEOUTS);

/* anonymous pipes */
BOOL WINAPI CreatePipe(PHANDLE, PHANDLE, LPSECURITY_ATTRIBUTES, DWORD);
BOOL WINAPI PeekNamedPipe(HANDLE, LPVOID, DWORD, LPDWORD, LPDWORD, LPDWORD);

/* the environment, the user, the speaker */
LPSTR WINAPI dmodem_GetEnvironmentStringsA(void);
BOOL WINAPI dmodem_FreeEnvironmentStringsA(LPSTR);
#define GetEnvironmentStrings dmodem_GetEnvironmentStringsA
#define FreeEnvironmentStrings dmodem_FreeEnvironmentStringsA
BOOL WINAPI GetUserNameA(LPSTR, LPDWORD);
#ifndef UNICODE
#define GetUserName GetUserNameA
#endif
BOOL WINAPI Beep(DWORD, DWORD);

#ifdef __cplusplus
}
#endif
#endif
