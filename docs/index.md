---
title: D-Modem desktop app
hide:
  - navigation
  - toc
---

<div class="suite-hero" markdown>
<img src="images/dmodem.svg" alt="">
<div markdown>

# D-Modem desktop app

**D-Modem on the desktop: a V.92 modem in a window, from Windows 95 to
Windows 11, on macOS and on Linux.**

The app has the modem built in. You do not need modem hardware or a
separate `dmodemd`. Dial a BBS, a fax machine or an ISP from a SIP call,
a USB modem or a hardware modem.

[Download the nightly build](download.md){ .md-button .md-button--primary }
[Source on GitHub](https://github.com/D-Modem/gui){ .md-button }

</div>
</div>

<img src="images/windows-settings.png" width="49%" alt="The Settings page of the app"> <img src="images/windows-setup-screen.png" width="49%" alt="The Terminal of the app with the setup screen of the modem">

## What the app does

<div class="grid cards" markdown>

-   **Modem**

    ---

    The state of the call. Dial, answer and hang up. Select the line that
    the modem uses: a SIP account, a USB modem, or a modem that already
    runs.

-   **Phone**

    ---

    Voice calls through the sound card, with a keypad. An answering machine
    can take messages on this computer. It is disabled by default.

-   **Terminal**

    ---

    The AT console and the setup screens of the modem (`AT!CF`, `AT!CT`).
    The emulator has xterm, DEC VT52 to VT525, ANSI-BBS, PETSCII, ATASCII,
    Viewdata, Minitel and Tektronix 4014.

-   **Remote**

    ---

    Log in to a server by SSH, Telnet or rlogin. SSH is built in. It uses
    the keys and the agent of OpenSSH and PuTTY.

-   **Settings**

    ---

    Each setting of the modem as a native control, from `AT!M`. You do not
    have to type AT commands.

-   **Call Stats**

    ---

    The rate, the modulation, the error control and the line figures of
    the last call (`AT#UD`, `ATI11`).

</div>

[All the features](features.md){ .md-button }

<img src="images/windows-modem.png" width="49%" alt="The Modem page of the app"> <img src="images/windows-phone.png" width="49%" alt="The Phone page of the app as it takes a message">

## One Win32 app, one library

The app is plain Win32, built with
[clang-retro](https://clang-retro.github.io/). The same Win32 code also
runs on Linux and macOS, over the portable Win32 layer of
[clang-retro MFC](https://github.com/clang-retro/mfc). The app is a thin
shell over one small C library, libdmodemctl. Thus it operates in the
same way on each system.

| | Windows | macOS | Linux |
| --- | --- | --- | --- |
| Toolkit | Win32, C++ | Win32, over the Cocoa layer of clang-retro MFC | Win32, over the X11 and Wayland layer of clang-retro MFC |
| Runs on | Windows 95 and later | arm64 (Apple silicon) and x86_64 (Intel Mac). Also Mac OS X 10.4 and later on PowerPC and i386 | x86_64 |
| Built-in modem | Yes | Yes | Yes |

Refer to [Design](design.md).

## Start here

- **Use the app:** [download the nightly build](download.md) for your
  system.
- **Build the app:** refer to [Building](building.md).
- **Use the modem without dmodemd:** refer to
  [The built-in modem](embedded-modem.md).
- **Learn how the app is built:** refer to [Design](design.md) and to
  the API of [libdmodemctl](https://github.com/D-Modem/gui/blob/main/core/include/dmodemctl.h).

## Part of the D-Modem suite

The desktop app is one part of the [D-Modem suite](https://d-modem.github.io/).
The suite has the softmodem ([D-Modem](https://d-modem.github.io/D-Modem/)),
the [app for Android and iOS](https://d-modem.github.io/app/), the
[pico-serial boards](https://d-modem.github.io/hardware/), and the
servers that answer callers ([login](https://d-modem.github.io/login/) and
[bbs](https://d-modem.github.io/bbs/)).

## Licence

AGPL-3.0-only ([LICENSE](../LICENSE)), or a commercial licence from the
D-Modem Authors ([COMMERCIAL-LICENSE.md](../COMMERCIAL-LICENSE.md)).
Contributions come with a contributor agreement
([CONTRIBUTING.md](../CONTRIBUTING.md)).
