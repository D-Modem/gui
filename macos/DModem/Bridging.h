/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * Bridging.h - libdmodemctl, libdmodemssh and (when the build has it) the
 * built-in modem's libdmodem_embed, for Swift
 */
#include "dmodemctl.h"
#include "dmodemssh.h"
// (the header path is set when the build has the built-in modem:
// ../build/macos-embed.xcconfig)
#if __has_include("dmodem_embed.h")
#include "dmodem_embed.h"
#endif
