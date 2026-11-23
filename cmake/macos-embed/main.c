/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * main.c - dmodem_embed_link (cmake/MacosEmbed.cmake): a program that
 * links libdmodem_embed, so that CMake gives the full link command of
 * the library
 */
#include "dmodem_embed.h"

/* Uses the library, so that the linker includes it. */
int main(void)
{
	return dmodem_embed_version() ? 0 : 1;
}
