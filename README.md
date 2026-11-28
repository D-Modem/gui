<img src=".github/banner.png" alt="D-Modem/gui: D-Modem on the desktop" width="100%">

# D-Modem desktop app

The D-Modem desktop app is one Win32 app for Windows, macOS and Linux. The
site of the app is <https://d-modem.github.io/gui/>.

The app has the modem built in, so it needs no dmodemd (refer to
[The built-in modem](docs/embedded-modem.md)). The app shows the modem as a
desktop program. You can dial and answer from a window, change the settings
without AT commands, and see the results of the last call. The app is a
thin Win32 shell over one small C library, libdmodemctl.

- **Modem**: the state of the call, dial, answer and hang up, and the line that the modem uses
- **Phone**: voice calls through the sound card (dial, answer, keypad), and an
  answering machine that takes messages on this computer when you enable it (disabled by default)
- **Terminal**: the AT console, and the setup screens of the modem (AT!CF, AT!CT)
- **Remote**: log in to a server by SSH, Telnet or rlogin, in a separate terminal. SSH is built in, on libssh, and it uses the keys and the agent of OpenSSH and PuTTY
- **Terminal types**: the terminal emulator of the D-Modem app, ported to C: xterm, DEC VT52 to VT525, ANSI-BBS, PETSCII, ATASCII, Viewdata, Minitel and Tektronix 4014, with sixel, ReGIS and Tektronix graphics
- **HyperTerminal items**: file transfers (ZMODEM, YMODEM, XMODEM and Kermit), copy and capture, and the ASCII Setup, Terminal Settings and Call menu of HyperTerminal. Refer to [Features](docs/features.md)
- **Settings**: each modem setting as a native control, from AT!M
- **Call Stats**: the rate, modulation, error control and line figures of the last call (AT#UD, ATI11)
- **Line settings**: a SIP account, a USB modem, or a modem that already runs (dmodemd, or a hardware modem such as the MultiTech MT9234MU)

| | Windows | macOS | Linux |
| --- | --- | --- | --- |
| Toolkit | Win32, C++ | Win32, over the Cocoa layer of [clang-retro MFC](https://github.com/clang-retro/mfc) | Win32, over the X11 and Wayland layer of clang-retro MFC |
| Built with | [clang-retro](https://clang-retro.github.io/), MinGW or MSVC | clang | clang |
| Runs on | Windows 95 and later | arm64 (Apple silicon) and x86_64 (Intel Mac) | x86_64 |

The source code of the app is the same on each system. Refer to "The
Windows app on Linux and macOS" in [Design](docs/design.md).

<img src="docs/images/windows-settings.png" width="49%" alt="The Settings page of the app"> <img src="docs/images/windows-setup-screen.png" width="49%" alt="The Terminal of the app with the setup screen of the modem">

<img src="docs/images/windows-modem.png" width="49%" alt="The Modem page of the app"> <img src="docs/images/windows-phone.png" width="49%" alt="The Phone page of the app as it takes a message">

## Nightly builds

The [nightly release](https://github.com/D-Modem/gui/releases/tag/nightly)
has the app from the newest build of `main`: for Windows 95 and later, for
Windows 7 and later, for Linux, and for macOS. A workflow
(`.github/workflows/nightly.yml`) replaces the release one time each day.
When `main` has no new commits, the release does not change. When the
main branch of D-Modem changes, a workflow moves the built-in modem to it
and builds the app again (refer to [Building](docs/building.md)).

These builds are not stable releases. The files are not signed. Thus
Windows and macOS can show a warning before the app starts.

## Quick start

Linux (get [clang-retro MFC](https://github.com/clang-retro/mfc) first):

    git submodule update --init
    sudo apt install cmake ninja-build clang llvm libx11-dev \
        libusb-1.0-0-dev libasound2-dev
    cmake -S . -B build -DDMODEM_GUI_WINPORT=ON -DCLANG_RETRO_MFC=/path/to/mfc \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
    cmake --build build --target dmodem-win32 && ./build/winport/dmodem-win32

For Windows (with clang-retro) and macOS, refer to
[Building](docs/building.md).

## Documentation

- [The site](https://d-modem.github.io/gui/): the home page, the downloads and all the pages below
- [Features](docs/features.md): each page and menu of the app
- [Building](docs/building.md): Windows with clang-retro, and the app on Linux and macOS
- [The built-in modem](docs/embedded-modem.md): the modem inside the app, with no dmodemd
- [Design](docs/design.md): how the app is built, hardware modems (the MultiTech MT9234MU), the Windows 9x notes, and the app on Linux and macOS
- [libdmodemctl](core/include/dmodemctl.h): the API of the library
- [Documentation style](docs/STYLE.md): the rules for ASD-STE100 Simplified Technical English

## The D-Modem suite

| Repository | What it is |
| --- | --- |
| [D-Modem](https://github.com/D-Modem/D-Modem) | The open V.92 softmodem: dmodemd, libdmodem and the Asterisk, FreeSWITCH and ARI modules |
| [app](https://github.com/D-Modem/app) | The D-Modem app for Android and iOS: a dial-up modem on a SIP call |
| [gui](https://github.com/D-Modem/gui) | D-Modem on the desktop: one Win32 app for Windows, macOS and Linux |
| [hardware](https://github.com/D-Modem/hardware) | pico-serial: RP2040 serial adapters and phone-line USB sticks |
| [login](https://github.com/D-Modem/login) | dmodem-login: the login, menu and LDAP provider for dial-up callers |
| [bbs](https://github.com/D-Modem/bbs) | A bulletin board for telnet, SSH, TN3270 and modem callers |
| [brand](https://github.com/D-Modem/brand) | Logos, colours, type and the naming guide |

## Licence

AGPL-3.0-only ([LICENSE](LICENSE)), or a commercial licence from the D-Modem Authors ([COMMERCIAL-LICENSE.md](COMMERCIAL-LICENSE.md)). Contributions come with a contributor agreement ([CONTRIBUTING.md](CONTRIBUTING.md)).
