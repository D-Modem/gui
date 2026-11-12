/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * ppk.h - PuTTY's private key files (.ppk) read, for libdmodemssh.
 *
 * This file and ppk.c are the same in D-Modem/gui (ssh/) and D-Modem/app
 * (native-ssh/): change both.
 */
#ifndef DMODEM_PPK_H
#define DMODEM_PPK_H

#include <stddef.h>
#include <stdint.h>

/* whether text is a .ppk (PuTTY-User-Key-File-...) */
int dmc_ppk_is(const char *text);

/* a .ppk (version 2 or 3) as OpenSSH's private key text, unencrypted
   (malloc'd; wipe it before it's freed): 0; -1 it can't be read; -2 it
   needs a passphrase, or the one given is wrong */
int dmc_ppk_to_openssh(const char *text, const char *passphrase, char **out);

/* whether a private key's text is encrypted (a passphrase needed):
   PuTTY's .ppk, OpenSSH's own, or PEM's */
int dmc_key_text_encrypted(const char *text);

/* Argon2 (type 0 Argon2d, 1 Argon2i, 2 Argon2id; version 0x13) of pwd
   and salt, with an optional secret and associated data, into out
   (outlen bytes): 0, or -1 */
int dmc_argon2(int type, uint32_t mem_kib, uint32_t passes, uint32_t lanes,
	       const unsigned char *pwd, size_t pwdlen,
	       const unsigned char *salt, size_t saltlen,
	       const unsigned char *secret, size_t secretlen,
	       const unsigned char *ad, size_t adlen,
	       unsigned char *out, uint32_t outlen);

#endif
