/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * mmreg.h - the MP3 format of the ACM, as the app's audio.cpp names it
 */
#ifndef DMODEM_WINPORT_MMREG_H
#define DMODEM_WINPORT_MMREG_H
#include <mmsystem.h>

#define WAVE_FORMAT_MPEGLAYER3 0x0055
#define MPEGLAYER3_WFX_EXTRA_BYTES 12
#define MPEGLAYER3_ID_MPEG 1
#define MPEGLAYER3_FLAG_PADDING_OFF 0x00000002

#pragma pack(push, 1)
typedef struct mpeglayer3waveformat_tag {
	WAVEFORMATEX wfx;
	WORD wID;
	DWORD fdwFlags;
	WORD nBlockSize;
	WORD nFramesPerBlock;
	WORD nCodecDelay;
} MPEGLAYER3WAVEFORMAT, *LPMPEGLAYER3WAVEFORMAT;
#pragma pack(pop)
#endif
