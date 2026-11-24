#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
#
# link-capture.sh - the linker launcher of dmodem_embed_link
# (cmake/MacosEmbed.cmake). It writes the directory and the link
# command, one argument on each line, to <file>. Then it runs the
# command.
#
# Usage: link-capture.sh <file> <command> [<argument>...]

out=$1
shift
{
	pwd
	printf '%s\n' "$@"
} > "$out"
exec "$@"
