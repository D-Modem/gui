/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * util.c - growable byte buffers and string helpers
 */
#include <stdlib.h>
#include <string.h>

#include "util.h"

static void grow(dmc_buf *b, int need)
{
	if (b->n + need + 1 <= b->cap)
		return;
	int cap = b->cap ? b->cap : 64;
	while (cap < b->n + need + 1)
		cap *= 2;
	char *p = realloc(b->p, (size_t)cap);
	if (!p)
		abort();
	b->p = p;
	b->cap = cap;
}

void buf_add(dmc_buf *b, const char *s, int n)
{
	if (n <= 0)
		return;
	grow(b, n);
	memcpy(b->p + b->n, s, (size_t)n);
	b->n += n;
	b->p[b->n] = '\0';
}

void buf_adds(dmc_buf *b, const char *s)
{
	buf_add(b, s, (int)strlen(s));
}

void buf_addc(dmc_buf *b, char c)
{
	buf_add(b, &c, 1);
}

void buf_drop(dmc_buf *b, int n)
{
	if (n >= b->n) {
		buf_clear(b);
		return;
	}
	memmove(b->p, b->p + n, (size_t)(b->n - n));
	b->n -= n;
	b->p[b->n] = '\0';
}

void buf_pop(dmc_buf *b)
{
	if (b->n > 0)
		b->p[--b->n] = '\0';
}

void buf_clear(dmc_buf *b)
{
	b->n = 0;
	if (b->p)
		b->p[0] = '\0';
}

void buf_free(dmc_buf *b)
{
	free(b->p);
	b->p = NULL;
	b->n = b->cap = 0;
}

int buf_take(dmc_buf *b, char *out, int max)
{
	int n = b->n < max ? b->n : max;
	if (n <= 0)
		return 0;
	memcpy(out, b->p, (size_t)n);
	buf_drop(b, n);
	return n;
}

const char *buf_str(dmc_buf *b)
{
	return b->p ? b->p : "";
}

char *dup_n(const char *s, size_t n)
{
	char *p = malloc(n + 1);
	if (!p)
		abort();
	memcpy(p, s, n);
	p[n] = '\0';
	return p;
}

char *dup_str(const char *s)
{
	return dup_n(s ? s : "", strlen(s ? s : ""));
}

static char lower(char c)
{
	return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

int same_ci(const char *s, const char *t)
{
	while (*s && *t && lower(*s) == lower(*t)) {
		s++;
		t++;
	}
	return !*s && !*t;
}

int starts_ci(const char *s, const char *p)
{
	while (*p) {
		if (lower(*s) != lower(*p))
			return 0;
		s++;
		p++;
	}
	return 1;
}

void set_str(char **p, const char *s)
{
	free(*p);
	*p = dup_str(s);
}
