/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * util.h - growable byte buffers and string helpers shared by the
 *          library's parts
 */
#ifndef DMC_UTIL_H
#define DMC_UTIL_H

#include <stddef.h>

/* a growable run of bytes, always ended by a '\0' past n */
typedef struct dmc_buf {
	char *p;
	int n, cap;
} dmc_buf;

void buf_add(dmc_buf *b, const char *s, int n);
void buf_adds(dmc_buf *b, const char *s);
void buf_addc(dmc_buf *b, char c);
/* the last byte taken off (none: nothing) */
void buf_pop(dmc_buf *b);
/* the first n bytes taken out */
void buf_drop(dmc_buf *b, int n);
void buf_clear(dmc_buf *b);
void buf_free(dmc_buf *b);
/* up to max bytes moved into out: how many */
int buf_take(dmc_buf *b, char *out, int max);
/* its text ("" when empty) */
const char *buf_str(dmc_buf *b);

/* a copy of s (or of its first n bytes) */
char *dup_str(const char *s);
char *dup_n(const char *s, size_t n);
/* s and t the same, ignoring ASCII case */
int same_ci(const char *s, const char *t);
/* s begins with p, ignoring ASCII case */
int starts_ci(const char *s, const char *p);
/* *p set to a copy of s (the old one freed) */
void set_str(char **p, const char *s);

#endif
