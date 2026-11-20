/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * zstd_codec.h - Zstandard (RFC 8878), whole frames, in plain C: the
 *                encoder makes one frame of a chunk (single segment, its
 *                content size and checksum in it, no dictionary), as
 *                ZMODEM with zstd sends a file (docs/zmodem-zstd.md of
 *                D-Modem); the decoder takes any frame up to its limit.
 *                A port of D-Modem's link/zstd_codec.cpp (and of the
 *                app's Zstd.kt): a hash chain over the chunk with a lazy
 *                match, the repeat offsets, literals raw, RLE or Huffman
 *                coded, each sequence table the predefined or a new one.
 *                Named so that it does not hide the system's <zstd.h>.
 */
#ifndef DMC_ZSTD_CODEC_H
#define DMC_ZSTD_CODEC_H

#include <stddef.h>
#include <stdint.h>

#define ZC_MAGIC 0xFD2FB528u

/* data[0..n) as one frame (malloc'd, its length in *out_n); NULL when
   memory ran out */
unsigned char *zc_encode(const unsigned char *data, int n, int *out_n);

/* the content of the frame frame[0..n) (all of it, no more), malloc'd,
   its length in *out_n (a frame of no content gives a block of 1 byte
   that is not part of it); NULL if it is not a frame, has a dictionary,
   a window or content over max, bad data or a bad checksum (why, if
   not NULL, says which) */
unsigned char *zc_decode(const unsigned char *frame, int n, int max, int *out_n,
			 const char **why);

/* the length of the frame at the start of b[0..n) from its headers (RFC
   8878 3.1.1.1 and 3.1.1.2): 0 while not all of it is there, -1 if it is
   not a frame that ZMODEM with zstd takes (a frame of no more than max
   bytes of content, without a dictionary, with its content size and
   checksum) */
int zc_frame_len(const unsigned char *b, int n, int max);

/* XXH64 (seed 0), Zstandard's content checksum */
uint64_t zc_xxh64(const unsigned char *b, size_t n);

#endif
