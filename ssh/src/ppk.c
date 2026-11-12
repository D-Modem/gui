/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * ppk.c - PuTTY's private key files (.ppk), versions 2 and 3, read and
 *         turned into OpenSSH's form, for libssh to load.
 *
 * A .ppk's public blob and private blob are SSH's own wire form, so the
 * key is only rearranged into an OpenSSH private key (unencrypted: it is
 * read straight back and never kept).  An encrypted one is AES-256-CBC,
 * its key from SHA-1 of the passphrase (version 2) or from Argon2
 * (version 3); either way a MAC over the whole file says whether the
 * passphrase was right.  The hashes and ciphers are here, small and
 * plain, so that this reads the same whatever libssh was built on.
 *
 * This file and ppk.h are the same in D-Modem/gui (ssh/) and D-Modem/app
 * (native-ssh/): change both.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ppk.h"

/* -- bytes -- */

typedef struct {
	unsigned char *p;
	size_t len, cap;
	int bad;
} out_t;

/* n bytes put at the end of o */
static void put(out_t *o, const void *b, size_t n)
{
	if (o->bad)
		return;
	if (o->len + n > o->cap) {
		size_t c = o->cap ? o->cap * 2 : 256;
		unsigned char *q;
		while (c < o->len + n)
			c *= 2;
		q = realloc(o->p, c);
		if (!q) {
			o->bad = 1;
			return;
		}
		o->p = q;
		o->cap = c;
	}
	memcpy(o->p + o->len, b, n);
	o->len += n;
}

/* a uint32, big-endian */
static void put32(out_t *o, uint32_t v)
{
	unsigned char b[4];
	b[0] = (unsigned char)(v >> 24);
	b[1] = (unsigned char)(v >> 16);
	b[2] = (unsigned char)(v >> 8);
	b[3] = (unsigned char)v;
	put(o, b, 4);
}

/* an SSH string: its length, then its bytes */
static void put_str(out_t *o, const void *b, size_t n)
{
	put32(o, (uint32_t)n);
	put(o, b, n);
}

/* o's bytes wiped and freed */
static void out_free(out_t *o)
{
	if (o->p) {
		memset(o->p, 0, o->cap);
		free(o->p);
	}
	memset(o, 0, sizeof *o);
}

/* a big-endian uint32 at b */
static uint32_t get32(const unsigned char *b)
{
	return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 |
	       (uint32_t)b[2] << 8 | b[3];
}

/* a little-endian uint32 at b */
static void le32(unsigned char *b, uint32_t v)
{
	b[0] = (unsigned char)v;
	b[1] = (unsigned char)(v >> 8);
	b[2] = (unsigned char)(v >> 16);
	b[3] = (unsigned char)(v >> 24);
}

/* an SSH string read from *b (left *n): its bytes and length, or -1 */
static int get_str(const unsigned char **b, size_t *n,
		   const unsigned char **s, size_t *len)
{
	uint32_t l;
	if (*n < 4)
		return -1;
	l = get32(*b);
	if (l > *n - 4)
		return -1;
	*s = *b + 4;
	*len = l;
	*b += 4 + l;
	*n -= 4 + l;
	return 0;
}

/* -- SHA-1 and SHA-256 -- */

typedef struct {
	uint32_t h[8];
	uint64_t n;
	unsigned char b[64];
	int big;	/* SHA-256 */
} sha_t;

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static const uint32_t K256[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
	0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
	0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
	0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
	0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
	0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

/* one 64-byte block into the state */
static void sha_block(sha_t *c, const unsigned char *p)
{
	uint32_t w[80], a, b, d, e, f, g, h, cc, t1, t2;
	int i;

	for (i = 0; i < 16; i++)
		w[i] = get32(p + 4 * i);
	a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4];
	if (!c->big) {
		for (i = 16; i < 80; i++)
			w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
		for (i = 0; i < 80; i++) {
			if (i < 20)
				t1 = ((b & cc) | (~b & d)) + 0x5a827999;
			else if (i < 40)
				t1 = (b ^ cc ^ d) + 0x6ed9eba1;
			else if (i < 60)
				t1 = ((b & cc) | (b & d) | (cc & d)) + 0x8f1bbcdc;
			else
				t1 = (b ^ cc ^ d) + 0xca62c1d6;
			t1 += ROL(a, 5) + e + w[i];
			e = d; d = cc; cc = ROL(b, 30); b = a; a = t1;
		}
		c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
		return;
	}
	f = c->h[5]; g = c->h[6]; h = c->h[7];
	for (i = 16; i < 64; i++) {
		uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	for (i = 0; i < 64; i++) {
		t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) +
		     K256[i] + w[i];
		t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
		h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
	}
	c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
	c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

/* a hash begun: SHA-256 when big, else SHA-1 */
static void sha_init(sha_t *c, int big)
{
	static const uint32_t i1[5] = {
		0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0
	};
	static const uint32_t i2[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
	};
	memset(c, 0, sizeof *c);
	c->big = big;
	memcpy(c->h, big ? i2 : i1, big ? sizeof i2 : sizeof i1);
}

/* n bytes hashed */
static void sha_add(sha_t *c, const void *data, size_t n)
{
	const unsigned char *p = data;
	while (n) {
		size_t at = (size_t)(c->n % 64), k = 64 - at;
		if (k > n)
			k = n;
		memcpy(c->b + at, p, k);
		c->n += k;
		p += k;
		n -= k;
		if (c->n % 64 == 0)
			sha_block(c, c->b);
	}
}

/* the digest (20 or 32 bytes) into out */
static void sha_done(sha_t *c, unsigned char *out)
{
	uint64_t bits = c->n * 8;
	unsigned char pad = 0x80, z = 0, l[8];
	int i;

	sha_add(c, &pad, 1);
	while (c->n % 64 != 56)
		sha_add(c, &z, 1);
	for (i = 0; i < 8; i++)
		l[i] = (unsigned char)(bits >> (56 - 8 * i));
	sha_add(c, l, 8);
	for (i = 0; i < (c->big ? 8 : 5); i++) {
		out[4 * i] = (unsigned char)(c->h[i] >> 24);
		out[4 * i + 1] = (unsigned char)(c->h[i] >> 16);
		out[4 * i + 2] = (unsigned char)(c->h[i] >> 8);
		out[4 * i + 3] = (unsigned char)c->h[i];
	}
	memset(c, 0, sizeof *c);
}

/* HMAC-SHA-256 (big) or HMAC-SHA-1 of data with key, into out */
static void hmac(int big, const unsigned char *key, size_t klen,
		 const unsigned char *data, size_t n, unsigned char *out)
{
	unsigned char k[64], pad[64], inner[32];
	sha_t c;
	int i;

	memset(k, 0, sizeof k);
	if (klen > 64) {
		sha_init(&c, big);
		sha_add(&c, key, klen);
		sha_done(&c, k);
	} else if (klen) {
		memcpy(k, key, klen);
	}
	for (i = 0; i < 64; i++)
		pad[i] = k[i] ^ 0x36;
	sha_init(&c, big);
	sha_add(&c, pad, 64);
	sha_add(&c, data, n);
	sha_done(&c, inner);
	for (i = 0; i < 64; i++)
		pad[i] = k[i] ^ 0x5c;
	sha_init(&c, big);
	sha_add(&c, pad, 64);
	sha_add(&c, inner, big ? 32 : 20);
	sha_done(&c, out);
	memset(k, 0, sizeof k);
	memset(pad, 0, sizeof pad);
	memset(inner, 0, sizeof inner);
}

/* -- AES-256, decryption, in CBC -- */

typedef struct {
	unsigned char sbox[256], inv[256];
	unsigned char rk[240];	/* 15 round keys */
} aes_t;

/* x times 2 in GF(2^8) */
static unsigned char xt(unsigned char x)
{
	return (unsigned char)((x << 1) ^ ((x & 0x80) ? 0x1b : 0));
}

/* a times b in GF(2^8) */
static unsigned char gmul(unsigned char a, unsigned char b)
{
	unsigned char r = 0;
	while (b) {
		if (b & 1)
			r ^= a;
		a = xt(a);
		b >>= 1;
	}
	return r;
}

/* the S-boxes made, and the key's schedule */
static void aes_init(aes_t *a, const unsigned char *key)
{
	unsigned char p = 1, q = 1, t[4], rcon = 1;
	int i, j;

	/* (the S-box from the multiplicative inverse and its affine map) */
	do {
		unsigned char x;
		p = (unsigned char)(p ^ (p << 1) ^ ((p & 0x80) ? 0x1b : 0));
		q ^= q << 1;
		q ^= q << 2;
		q ^= q << 4;
		if (q & 0x80)
			q ^= 0x09;
		x = (unsigned char)(q ^ (q << 1 | q >> 7) ^ (q << 2 | q >> 6) ^
				    (q << 3 | q >> 5) ^ (q << 4 | q >> 4));
		a->sbox[p] = (unsigned char)(x ^ 0x63);
	} while (p != 1);
	a->sbox[0] = 0x63;
	for (i = 0; i < 256; i++)
		a->inv[a->sbox[i]] = (unsigned char)i;

	memcpy(a->rk, key, 32);
	for (i = 32; i < 240; i += 4) {
		memcpy(t, a->rk + i - 4, 4);
		if (i % 32 == 0) {
			unsigned char u = t[0];
			t[0] = (unsigned char)(a->sbox[t[1]] ^ rcon);
			t[1] = a->sbox[t[2]];
			t[2] = a->sbox[t[3]];
			t[3] = a->sbox[u];
			rcon = xt(rcon);
		} else if (i % 32 == 16) {
			for (j = 0; j < 4; j++)
				t[j] = a->sbox[t[j]];
		}
		for (j = 0; j < 4; j++)
			a->rk[i + j] = a->rk[i - 32 + j] ^ t[j];
	}
}

/* one block decrypted in place */
static void aes_decrypt(const aes_t *a, unsigned char *s)
{
	unsigned char t[16];
	int r, i, c;

	for (i = 0; i < 16; i++)
		s[i] ^= a->rk[224 + i];
	for (r = 13; r >= 0; r--) {
		/* inverse ShiftRows and SubBytes */
		for (c = 0; c < 4; c++)
			for (i = 0; i < 4; i++)
				t[4 * ((c + i) % 4) + i] = a->inv[s[4 * c + i]];
		for (i = 0; i < 16; i++)
			s[i] = t[i] ^ a->rk[16 * r + i];
		if (r == 0)
			break;
		/* inverse MixColumns */
		for (c = 0; c < 4; c++) {
			unsigned char *m = s + 4 * c, b0 = m[0], b1 = m[1], b2 = m[2], b3 = m[3];
			m[0] = gmul(b0, 14) ^ gmul(b1, 11) ^ gmul(b2, 13) ^ gmul(b3, 9);
			m[1] = gmul(b0, 9) ^ gmul(b1, 14) ^ gmul(b2, 11) ^ gmul(b3, 13);
			m[2] = gmul(b0, 13) ^ gmul(b1, 9) ^ gmul(b2, 14) ^ gmul(b3, 11);
			m[3] = gmul(b0, 11) ^ gmul(b1, 13) ^ gmul(b2, 9) ^ gmul(b3, 14);
		}
	}
	memset(t, 0, sizeof t);
}

/* n bytes (whole blocks) decrypted in place, AES-256-CBC */
static void cbc_decrypt(const unsigned char *key, const unsigned char *iv,
			unsigned char *p, size_t n)
{
	aes_t *a = malloc(sizeof *a);
	unsigned char prev[16], cur[16];
	size_t i;
	int j;

	if (!a)
		return;
	aes_init(a, key);
	memcpy(prev, iv, 16);
	for (i = 0; i + 16 <= n; i += 16) {
		memcpy(cur, p + i, 16);
		aes_decrypt(a, p + i);
		for (j = 0; j < 16; j++)
			p[i + j] ^= prev[j];
		memcpy(prev, cur, 16);
	}
	memset(a, 0, sizeof *a);
	free(a);
}

/* -- BLAKE2b and Argon2 (RFC 9106) -- */

typedef struct {
	uint64_t h[8], t;
	unsigned char b[128];
	size_t at, outlen;
} b2_t;

static const uint64_t B2IV[8] = {
	0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
	0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
	0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL
};

static const unsigned char SIGMA[12][16] = {
	{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
	{ 14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 },
	{ 11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4 },
	{ 7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8 },
	{ 9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13 },
	{ 2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9 },
	{ 12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11 },
	{ 13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10 },
	{ 6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5 },
	{ 10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0 },
	{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
	{ 14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 }
};

#define ROR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

/* a little-endian uint64 at p */
static uint64_t ld64(const unsigned char *p)
{
	uint64_t v = 0;
	int i;
	for (i = 7; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

/* v put little-endian at p */
static void st64(unsigned char *p, uint64_t v)
{
	int i;
	for (i = 0; i < 8; i++)
		p[i] = (unsigned char)(v >> (8 * i));
}

/* BLAKE2b's compression of the buffered block (last: the final one) */
static void b2_compress(b2_t *c, int last)
{
	uint64_t v[16], m[16];
	int i, r;

	for (i = 0; i < 16; i++)
		m[i] = ld64(c->b + 8 * i);
	for (i = 0; i < 8; i++) {
		v[i] = c->h[i];
		v[i + 8] = B2IV[i];
	}
	v[12] ^= c->t;
	if (last)
		v[14] = ~v[14];
#define B2G(a, b, cc, d, x, y) do { \
	v[a] = v[a] + v[b] + (x); v[d] = ROR64(v[d] ^ v[a], 32); \
	v[cc] = v[cc] + v[d]; v[b] = ROR64(v[b] ^ v[cc], 24); \
	v[a] = v[a] + v[b] + (y); v[d] = ROR64(v[d] ^ v[a], 16); \
	v[cc] = v[cc] + v[d]; v[b] = ROR64(v[b] ^ v[cc], 63); } while (0)
	for (r = 0; r < 12; r++) {
		const unsigned char *s = SIGMA[r];
		B2G(0, 4, 8, 12, m[s[0]], m[s[1]]);
		B2G(1, 5, 9, 13, m[s[2]], m[s[3]]);
		B2G(2, 6, 10, 14, m[s[4]], m[s[5]]);
		B2G(3, 7, 11, 15, m[s[6]], m[s[7]]);
		B2G(0, 5, 10, 15, m[s[8]], m[s[9]]);
		B2G(1, 6, 11, 12, m[s[10]], m[s[11]]);
		B2G(2, 7, 8, 13, m[s[12]], m[s[13]]);
		B2G(3, 4, 9, 14, m[s[14]], m[s[15]]);
	}
#undef B2G
	for (i = 0; i < 8; i++)
		c->h[i] ^= v[i] ^ v[i + 8];
}

/* BLAKE2b begun, outlen bytes (1 to 64) wanted, no key */
static void b2_init(b2_t *c, size_t outlen)
{
	memset(c, 0, sizeof *c);
	memcpy(c->h, B2IV, sizeof c->h);
	c->h[0] ^= 0x01010000ULL ^ (uint64_t)outlen;
	c->outlen = outlen;
}

/* n bytes hashed */
static void b2_add(b2_t *c, const void *data, size_t n)
{
	const unsigned char *p = data;
	while (n) {
		size_t k;
		if (c->at == 128) {
			c->t += 128;
			b2_compress(c, 0);
			c->at = 0;
		}
		k = 128 - c->at;
		if (k > n)
			k = n;
		memcpy(c->b + c->at, p, k);
		c->at += k;
		p += k;
		n -= k;
	}
}

/* the digest into out */
static void b2_done(b2_t *c, unsigned char *out)
{
	unsigned char h[64];
	int i;

	c->t += c->at;
	memset(c->b + c->at, 0, 128 - c->at);
	b2_compress(c, 1);
	for (i = 0; i < 8; i++)
		st64(h + 8 * i, c->h[i]);
	memcpy(out, h, c->outlen);
	memset(c, 0, sizeof *c);
}

/* Argon2's H': a hash of any length (n) of in, into out */
static void hprime(unsigned char *out, uint32_t n, const unsigned char *in,
		   size_t inlen)
{
	unsigned char len[4], v[64];
	b2_t c;

	le32(len, n);
	if (n <= 64) {
		b2_init(&c, n);
		b2_add(&c, len, 4);
		b2_add(&c, in, inlen);
		b2_done(&c, out);
		return;
	}
	b2_init(&c, 64);
	b2_add(&c, len, 4);
	b2_add(&c, in, inlen);
	b2_done(&c, v);
	memcpy(out, v, 32);
	out += 32;
	n -= 32;
	while (n > 64) {
		b2_init(&c, 64);
		b2_add(&c, v, 64);
		b2_done(&c, v);
		memcpy(out, v, 32);
		out += 32;
		n -= 32;
	}
	b2_init(&c, n);
	b2_add(&c, v, 64);
	b2_done(&c, out);
	memset(v, 0, sizeof v);
}

typedef struct { uint64_t v[128]; } block_t;

/* the permutation's quarter: BLAKE2b's G, its sums with Argon2's products */
#define FBLA(x, y) ((x) + (y) + 2 * ((x) & 0xffffffffULL) * ((y) & 0xffffffffULL))
#define AG(a, b, c, d) do { \
	a = FBLA(a, b); d = ROR64(d ^ a, 32); c = FBLA(c, d); b = ROR64(b ^ c, 24); \
	a = FBLA(a, b); d = ROR64(d ^ a, 16); c = FBLA(c, d); b = ROR64(b ^ c, 63); } while (0)

/* Argon2's P over 16 words, given by their indexes in the block */
static void perm(uint64_t *v, const int *i)
{
	AG(v[i[0]], v[i[4]], v[i[8]], v[i[12]]);
	AG(v[i[1]], v[i[5]], v[i[9]], v[i[13]]);
	AG(v[i[2]], v[i[6]], v[i[10]], v[i[14]]);
	AG(v[i[3]], v[i[7]], v[i[11]], v[i[15]]);
	AG(v[i[0]], v[i[5]], v[i[10]], v[i[15]]);
	AG(v[i[1]], v[i[6]], v[i[11]], v[i[12]]);
	AG(v[i[2]], v[i[7]], v[i[8]], v[i[13]]);
	AG(v[i[3]], v[i[4]], v[i[9]], v[i[14]]);
}

/* Argon2's G: next = P(prev ^ ref) ^ prev ^ ref (^ next's old self when xor) */
static void fill(const block_t *prev, const block_t *ref, block_t *next, int xor_)
{
	block_t r, t;
	int i, j, idx[16];

	for (i = 0; i < 128; i++)
		r.v[i] = prev->v[i] ^ ref->v[i];
	t = r;
	if (xor_)
		for (i = 0; i < 128; i++)
			t.v[i] ^= next->v[i];
	/* rows: 16 words each */
	for (i = 0; i < 8; i++) {
		for (j = 0; j < 16; j++)
			idx[j] = 16 * i + j;
		perm(r.v, idx);
	}
	/* columns: pairs of words down the rows */
	for (i = 0; i < 8; i++) {
		for (j = 0; j < 8; j++) {
			idx[2 * j] = 2 * i + 16 * j;
			idx[2 * j + 1] = 2 * i + 16 * j + 1;
		}
		perm(r.v, idx);
	}
	for (i = 0; i < 128; i++)
		next->v[i] = t.v[i] ^ r.v[i];
}

/* a 1024-byte block from its bytes */
static void block_load(block_t *b, const unsigned char *p)
{
	int i;
	for (i = 0; i < 128; i++)
		b->v[i] = ld64(p + 8 * i);
}

/* Argon2 (type 0 d, 1 i, 2 id; version 0x13) of pwd and salt into out
   (outlen bytes): 0, or -1 */
int dmc_argon2(int type, uint32_t mem_kib, uint32_t passes, uint32_t lanes,
	       const unsigned char *pwd, size_t pwdlen,
	       const unsigned char *salt, size_t saltlen,
	       const unsigned char *secret, size_t secretlen,
	       const unsigned char *ad, size_t adlen,
	       unsigned char *out, uint32_t outlen)
{
	unsigned char h0[72], w[4], blk[1024];
	uint32_t m, q, seg, pass, slice, lane, i;
	block_t *B, addr, input, zero;
	b2_t c;

	if (lanes < 1 || lanes > 0xffffff || passes < 1 || outlen < 4 ||
	    mem_kib < 8 * lanes || mem_kib > 4 * 1024 * 1024 || type < 0 || type > 2)
		return -1;
	m = mem_kib / (4 * lanes) * 4 * lanes;
	q = m / lanes;
	seg = q / 4;
	B = calloc(m, sizeof *B);
	if (!B)
		return -1;

	b2_init(&c, 64);
#define W32(v) do { le32(w, (uint32_t)(v)); b2_add(&c, w, 4); } while (0)
	W32(lanes); W32(outlen); W32(mem_kib); W32(passes); W32(0x13); W32(type);
	W32(pwdlen); b2_add(&c, pwd, pwdlen);
	W32(saltlen); b2_add(&c, salt, saltlen);
	W32(secretlen); b2_add(&c, secret, secretlen);
	W32(adlen); b2_add(&c, ad, adlen);
#undef W32
	b2_done(&c, h0);

	for (lane = 0; lane < lanes; lane++) {
		le32(h0 + 64, 0);
		le32(h0 + 68, lane);
		hprime(blk, 1024, h0, 72);
		block_load(&B[lane * q], blk);
		le32(h0 + 64, 1);
		hprime(blk, 1024, h0, 72);
		block_load(&B[lane * q + 1], blk);
	}

	memset(&zero, 0, sizeof zero);
	for (pass = 0; pass < passes; pass++) {
		for (slice = 0; slice < 4; slice++) {
			for (lane = 0; lane < lanes; lane++) {
				int indep = type == 1 || (type == 2 && pass == 0 && slice < 2);
				uint32_t start = pass == 0 && slice == 0 ? 2 : 0;
				uint32_t cur = lane * q + slice * seg + start;

				if (indep) {
					memset(&input, 0, sizeof input);
					input.v[0] = pass;
					input.v[1] = lane;
					input.v[2] = slice;
					input.v[3] = m;
					input.v[4] = passes;
					input.v[5] = (uint64_t)type;
				}
				for (i = start; i < seg; i++, cur++) {
					uint32_t prev = cur % q ? cur - 1 : cur + q - 1, ref;
					uint64_t rnd, rel, area, startpos, ref_lane, x;

					if (indep) {
						/* (a new block of addresses each 128) */
						if (i % 128 == 0 || (i == 2 && start == 2)) {
							input.v[6]++;
							fill(&zero, &input, &addr, 0);
							fill(&zero, &addr, &addr, 0);
						}
						rnd = addr.v[i % 128];
					} else {
						rnd = B[prev].v[0];
					}
					ref_lane = (rnd >> 32) % lanes;
					if (pass == 0 && slice == 0)
						ref_lane = lane;
					if (pass == 0) {
						if (ref_lane == lane)
							area = slice * seg + i - 1;
						else
							area = slice * seg - (i == 0 ? 1 : 0);
					} else {
						if (ref_lane == lane)
							area = q - seg + i - 1;
						else
							area = q - seg - (i == 0 ? 1 : 0);
					}
					x = rnd & 0xffffffffULL;
					x = x * x >> 32;
					rel = area - 1 - (area * x >> 32);
					startpos = pass != 0 && slice != 3 ? (uint64_t)(slice + 1) * seg : 0;
					ref = (uint32_t)((startpos + rel) % q);
					fill(&B[prev], &B[ref_lane * q + ref], &B[cur], pass != 0);
				}
			}
		}
	}

	addr = B[q - 1];
	for (lane = 1; lane < lanes; lane++)
		for (i = 0; i < 128; i++)
			addr.v[i] ^= B[lane * q + q - 1].v[i];
	for (i = 0; i < 128; i++)
		st64(blk + 8 * i, addr.v[i]);
	hprime(out, outlen, blk, 1024);

	memset(blk, 0, sizeof blk);
	memset(h0, 0, sizeof h0);
	memset(&addr, 0, sizeof addr);
	memset(B, 0, (size_t)m * sizeof *B);
	free(B);
	return 0;
}

/* -- the file -- */

/* a header's value (to the line's end) in text, or NULL */
static const char *header(const char *text, const char *name, size_t *len)
{
	size_t n = strlen(name);
	const char *p = text;

	while (p && *p) {
		if (!strncmp(p, name, n) && p[n] == ':' && p[n + 1] == ' ') {
			const char *v = p + n + 2;
			*len = strcspn(v, "\r\n");
			while (*len && (v[*len - 1] == ' ' || v[*len - 1] == '\t'))
				(*len)--;
			return v;
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
	return NULL;
}

/* a header's value as a number, or -1 */
static long header_num(const char *text, const char *name)
{
	size_t n;
	const char *v = header(text, name, &n);
	long r = 0;
	size_t i;

	if (!v || !n || n > 9)
		return -1;
	for (i = 0; i < n; i++) {
		if (v[i] < '0' || v[i] > '9')
			return -1;
		r = r * 10 + (v[i] - '0');
	}
	return r;
}

/* base64's value of ch, or -1 */
static int b64v(int ch)
{
	if (ch >= 'A' && ch <= 'Z')
		return ch - 'A';
	if (ch >= 'a' && ch <= 'z')
		return ch - 'a' + 26;
	if (ch >= '0' && ch <= '9')
		return ch - '0' + 52;
	if (ch == '+')
		return 62;
	if (ch == '/')
		return 63;
	return -1;
}

/* the lines-many lines of base64 after header name, decoded into o:
   0, or -1 */
static int block(const char *text, const char *name, out_t *o)
{
	size_t n;
	const char *p = header(text, name, &n);
	long lines = header_num(text, name);
	unsigned acc = 0;
	int bits = 0;

	if (!p || lines < 0 || lines > 10000)
		return -1;
	p = strchr(p, '\n');
	while (lines-- > 0) {
		if (!p)
			return -1;
		p++;
		for (; *p && *p != '\n'; p++) {
			int v;
			if (*p == '\r' || *p == '=')
				continue;
			v = b64v((unsigned char)*p);
			if (v < 0)
				return -1;
			acc = acc << 6 | (unsigned)v;
			bits += 6;
			if (bits >= 8) {
				unsigned char b;
				bits -= 8;
				b = (unsigned char)(acc >> bits);
				put(o, &b, 1);
			}
		}
	}
	acc = 0;
	return o->bad ? -1 : 0;
}

/* a hex header's bytes into out (at most max): their count, or -1 */
static int hex_header(const char *text, const char *name, unsigned char *out, int max)
{
	size_t n, i;
	const char *v = header(text, name, &n);

	if (!v || n % 2 || (int)(n / 2) > max)
		return -1;
	for (i = 0; i < n; i += 2) {
		int d[2], k;
		for (k = 0; k < 2; k++) {
			char ch = v[i + (size_t)k];
			d[k] = ch >= '0' && ch <= '9' ? ch - '0' :
			       ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 :
			       ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
			if (d[k] < 0)
				return -1;
		}
		out[i / 2] = (unsigned char)(d[0] << 4 | d[1]);
	}
	return (int)(n / 2);
}

/* whether text is a .ppk */
int dmc_ppk_is(const char *text)
{
	while (text && (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n'))
		text++;
	return text && !strncmp(text, "PuTTY-User-Key-File-", 20);
}

/* the private key rearranged as OpenSSH's (its private section's key
   fields) from PuTTY's public and private blobs: 0, or -1 */
static int key_fields(out_t *o, const unsigned char *pub, size_t publen,
		      const unsigned char *priv, size_t privlen)
{
	const unsigned char *type, *a, *b, *c, *d, *e, *f;
	size_t tl, al, bl, cl, dl, el, fl;

	if (get_str(&pub, &publen, &type, &tl))
		return -1;
	put_str(o, type, tl);
	if (tl == 7 && !memcmp(type, "ssh-rsa", 7)) {
		/* PuTTY's e, n and d, p, q, iqmp; OpenSSH's n, e, d, iqmp, p, q */
		if (get_str(&pub, &publen, &a, &al) || get_str(&pub, &publen, &b, &bl) ||
		    get_str(&priv, &privlen, &c, &cl) || get_str(&priv, &privlen, &d, &dl) ||
		    get_str(&priv, &privlen, &e, &el) || get_str(&priv, &privlen, &f, &fl))
			return -1;
		put_str(o, b, bl);
		put_str(o, a, al);
		put_str(o, c, cl);
		put_str(o, f, fl);
		put_str(o, d, dl);
		put_str(o, e, el);
	} else if (tl > 11 && !memcmp(type, "ecdsa-sha2-", 11)) {
		/* the curve, the point; the private scalar */
		if (get_str(&pub, &publen, &a, &al) || get_str(&pub, &publen, &b, &bl) ||
		    get_str(&priv, &privlen, &c, &cl))
			return -1;
		put_str(o, a, al);
		put_str(o, b, bl);
		put_str(o, c, cl);
	} else if (tl == 11 && !memcmp(type, "ssh-ed25519", 11)) {
		/* the public 32 bytes; the private seed (32), and OpenSSH's
		   seed and public together */
		unsigned char both[64];
		if (get_str(&pub, &publen, &a, &al) || al != 32 ||
		    get_str(&priv, &privlen, &c, &cl) || cl > 33 || cl < 1)
			return -1;
		/* (an mpint's leading zero, if PuTTY wrote one) */
		if (cl == 33) {
			if (c[0])
				return -1;
			c++;
			cl--;
		}
		if (cl != 32)
			return -1;
		put_str(o, a, 32);
		memcpy(both, c, 32);
		memcpy(both + 32, a, 32);
		put_str(o, both, 64);
		memset(both, 0, sizeof both);
	} else {
		return -1;
	}
	return 0;
}

/* OpenSSH's private key file, unencrypted, of the key in out (PEM text) */
static char *openssh_text(const unsigned char *pub, size_t publen,
			  const unsigned char *priv, size_t privlen,
			  const char *comment, size_t commentlen)
{
	static const char b64[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	out_t sec = { 0 }, all = { 0 };
	unsigned char pad = 1;
	char *text = NULL, *t;
	size_t i, n, col = 0;

	put32(&sec, 0x44536d21);
	put32(&sec, 0x44536d21);
	if (key_fields(&sec, pub, publen, priv, privlen)) {
		out_free(&sec);
		return NULL;
	}
	put_str(&sec, comment, commentlen);
	while (sec.len % 8)
		put(&sec, &pad, 1), pad++;

	put(&all, "openssh-key-v1", 15);
	put_str(&all, "none", 4);
	put_str(&all, "none", 4);
	put_str(&all, "", 0);
	put32(&all, 1);
	put_str(&all, pub, publen);
	put_str(&all, sec.p, sec.len);
	out_free(&sec);
	if (all.bad) {
		out_free(&all);
		return NULL;
	}

	n = (all.len + 2) / 3 * 4;
	text = malloc(n + n / 70 + 128);
	if (text) {
		t = text + sprintf(text, "-----BEGIN OPENSSH PRIVATE KEY-----\n");
		for (i = 0; i < all.len; i += 3) {
			uint32_t v = (uint32_t)all.p[i] << 16;
			if (i + 1 < all.len)
				v |= (uint32_t)all.p[i + 1] << 8;
			if (i + 2 < all.len)
				v |= all.p[i + 2];
			*t++ = b64[v >> 18 & 63];
			*t++ = b64[v >> 12 & 63];
			*t++ = i + 1 < all.len ? b64[v >> 6 & 63] : '=';
			*t++ = i + 2 < all.len ? b64[v & 63] : '=';
			col += 4;
			if (col == 68) {
				*t++ = '\n';
				col = 0;
			}
		}
		if (col)
			*t++ = '\n';
		strcpy(t, "-----END OPENSSH PRIVATE KEY-----\n");
	}
	out_free(&all);
	return text;
}

/* a .ppk (version 2 or 3) as OpenSSH's private key text, unencrypted
   (malloc'd; wipe it before it's freed): 0; -1 it can't be read; -2 it
   needs a passphrase, or the one given is wrong */
int dmc_ppk_to_openssh(const char *text, const char *passphrase, char **out)
{
	out_t pub = { 0 }, priv = { 0 }, mac = { 0 };
	const char *first, *alg, *enc, *comment;
	size_t alglen, enclen, commentlen, pl;
	unsigned char want[32], got[32], keys[80], salt[64];
	int version, encrypted, r = -1, i, maclen;

	*out = NULL;
	while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
		text++;
	if (!strncmp(text, "PuTTY-User-Key-File-2:", 22))
		version = 2;
	else if (!strncmp(text, "PuTTY-User-Key-File-3:", 22))
		version = 3;
	else
		return -1;
	first = text + 23;
	alg = first;
	alglen = strcspn(alg, "\r\n");
	while (alglen && alg[alglen - 1] == ' ')
		alglen--;
	enc = header(text, "Encryption", &enclen);
	comment = header(text, "Comment", &commentlen);
	if (!comment) {
		comment = "";
		commentlen = 0;
	}
	if (!enc)
		return -1;
	if (enclen == 4 && !memcmp(enc, "none", 4))
		encrypted = 0;
	else if (enclen == 10 && !memcmp(enc, "aes256-cbc", 10))
		encrypted = 1;
	else
		return -1;
	maclen = hex_header(text, "Private-MAC", want, sizeof want);
	if (maclen != (version == 3 ? 32 : 20))
		return -1;
	if (block(text, "Public-Lines", &pub) || block(text, "Private-Lines", &priv))
		goto done;
	if (encrypted && (!passphrase || !*passphrase)) {
		r = -2;
		goto done;
	}
	pl = encrypted ? strlen(passphrase) : 0;
	memset(keys, 0, sizeof keys);

	if (version == 3) {
		if (encrypted) {
			size_t kn;
			const char *kdf = header(text, "Key-Derivation", &kn);
			long mem = header_num(text, "Argon2-Memory");
			long passes = header_num(text, "Argon2-Passes");
			long par = header_num(text, "Argon2-Parallelism");
			int saltlen = hex_header(text, "Argon2-Salt", salt, sizeof salt);
			int type;
			if (!kdf || mem < 0 || passes < 0 || par < 0 || saltlen < 0)
				goto done;
			if (kn == 8 && !memcmp(kdf, "Argon2id", 8))
				type = 2;
			else if (kn == 7 && !memcmp(kdf, "Argon2i", 7))
				type = 1;
			else if (kn == 7 && !memcmp(kdf, "Argon2d", 7))
				type = 0;
			else
				goto done;
			if (dmc_argon2(type, (uint32_t)mem, (uint32_t)passes, (uint32_t)par,
				       (const unsigned char *)passphrase, pl,
				       salt, (size_t)saltlen, NULL, 0, NULL, 0, keys, 80))
				goto done;
		}
	} else {
		sha_t c;
		static const unsigned char z[4] = { 0, 0, 0, 0 }, one[4] = { 0, 0, 0, 1 };
		unsigned char h[20];
		if (encrypted) {
			sha_init(&c, 0);
			sha_add(&c, z, 4);
			sha_add(&c, passphrase, pl);
			sha_done(&c, h);
			memcpy(keys, h, 20);
			sha_init(&c, 0);
			sha_add(&c, one, 4);
			sha_add(&c, passphrase, pl);
			sha_done(&c, h);
			memcpy(keys + 20, h, 12);
		}
		/* (the MAC's key: SHA-1 of a phrase and the passphrase) */
		sha_init(&c, 0);
		sha_add(&c, "putty-private-key-file-mac-key", 30);
		if (encrypted)
			sha_add(&c, passphrase, pl);
		sha_done(&c, keys + 48);
		memset(h, 0, sizeof h);
	}

	if (encrypted) {
		if (priv.len % 16)
			goto done;
		{
			/* (version 2's IV: zeros) */
			static const unsigned char zero_iv[16] = { 0 };
			cbc_decrypt(keys, version == 3 ? keys + 32 : zero_iv, priv.p, priv.len);
		}
	}

	put_str(&mac, alg, alglen);
	put_str(&mac, enc, enclen);
	put_str(&mac, comment, commentlen);
	put_str(&mac, pub.p, pub.len);
	put_str(&mac, priv.p, priv.len);
	if (mac.bad)
		goto done;
	hmac(version == 3, keys + 48, version == 3 ? (encrypted ? 32 : 0) : 20,
	     mac.p, mac.len, got);
	{
		unsigned char diff = 0;
		for (i = 0; i < maclen; i++)
			diff |= (unsigned char)(got[i] ^ want[i]);
		if (diff) {
			r = encrypted ? -2 : -1;
			goto done;
		}
	}
	*out = openssh_text(pub.p, pub.len, priv.p, priv.len, comment, commentlen);
	r = *out ? 0 : -1;
done:
	memset(keys, 0, sizeof keys);
	memset(got, 0, sizeof got);
	out_free(&pub);
	out_free(&priv);
	out_free(&mac);
	return r;
}

/* whether a private key's text is encrypted (a passphrase needed):
   PuTTY's .ppk, OpenSSH's own, or PEM's */
int dmc_key_text_encrypted(const char *text)
{
	static const char begin[] = "-----BEGIN OPENSSH PRIVATE KEY-----";
	const char *p;
	size_t n;
	out_t o = { 0 };
	int r = 0;

	if (!text)
		return 0;
	if (dmc_ppk_is(text)) {
		p = header(text, "Encryption", &n);
		return p && !(n == 4 && !memcmp(p, "none", 4));
	}
	if (strstr(text, "Proc-Type: 4,ENCRYPTED") ||
	    strstr(text, "-----BEGIN ENCRYPTED PRIVATE KEY-----"))
		return 1;
	p = strstr(text, begin);
	if (!p)
		return 0;
	/* "openssh-key-v1", then the cipher's name: "none" or another */
	p = strchr(p + sizeof begin - 1, '\n');
	if (p) {
		unsigned acc = 0;
		int bits = 0;
		for (p++; *p && o.len < 64 && *p != '-'; p++) {
			int v = b64v((unsigned char)*p);
			if (v < 0)
				continue;
			acc = acc << 6 | (unsigned)v;
			bits += 6;
			if (bits >= 8) {
				unsigned char b;
				bits -= 8;
				b = (unsigned char)(acc >> bits);
				put(&o, &b, 1);
			}
		}
		if (o.len >= 23 && !memcmp(o.p, "openssh-key-v1", 15)) {
			uint32_t l = get32(o.p + 15);
			r = !(l == 4 && !memcmp(o.p + 19, "none", 4));
		}
	}
	out_free(&o);
	return r;
}
