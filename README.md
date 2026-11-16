<img src=".github/banner.png" alt="D-Modem/gui: D-Modem on the desktop" width="100%">

# D-Modem desktop apps

Native apps for D-Modem on Windows, macOS and Linux.

Each app starts dmodemd (or connects to a dmodemd that already runs). It
shows the modem as a desktop program. You can dial and answer from a window,
change the settings without AT commands, and see the results of the last
call. All three apps are thin native shells over one small C library,
libdmodemctl. Thus they operate in the same way.

- **Modem**: the state of the call, dial, answer and hang up, and the line that the modem uses
- **Phone**: voice calls through the sound card (dial, answer, keypad), and an
  answering machine that takes messages on this computer when you enable it (disabled by default)
- **Terminal**: the AT console, and the setup screens of the modem (AT!CF, AT!CT)
- **Remote**: log in to a server by SSH, Telnet or rlogin, in a separate terminal. SSH is built in, on libssh: it uses the keys, agent, config and known hosts of OpenSSH. It can use or import the `.ppk` keys of PuTTY. It uses Pageant on Windows, and an ssh-agent on Linux and macOS
- **Terminal types**: the two terminals use the emulator of the D-Modem app, ported to C:
  xterm, the VT52 to VT525 of DEC, ANSI-BBS (with Avatar, RIPscrip and Amiga ANSI),
  the PETSCII of the Commodores, the ATASCII of the Atari 8-bit and the ST, Viewdata,
  the Minitel and the Tektronix 4014. They have sixel, ReGIS and Tektronix
  graphics, and the font of each machine (the SyncTERM copies of their ROMs,
  LGPL-2.0-or-later: `core/licenses/syncterm-fonts-LGPL-2.0.txt`)
- **Settings**: each modem setting as a native control, from AT!M
- **Call Stats**: the rate, modulation, error control and line figures of the last call (AT#UD, ATI11)
- **Line settings**: a SIP account, a USB modem, or a modem that already runs
- **Windows 95 and later**: plain Win32, built with Open Watcom. On Windows 9x, the app connects to a modem through TCP or a COM port

| | Windows | macOS | Linux |
| --- | --- | --- | --- |
| Toolkit | Win32, C++ | SwiftUI | GTK 4, libadwaita |
| Runs on | Windows 95 and later | macOS 13 and later | GTK 4.10 and libadwaita 1.5 or later (Ubuntu 24.04, Fedora 40) |

<img src="docs/images/linux-settings.png" width="49%" alt="The Linux app's Settings page"> <img src="docs/images/windows-setup-screen.png" width="49%" alt="The Windows app's Terminal with the modem's setup screen">

<img src="docs/images/linux-phone.png" width="49%" alt="The Linux app's Phone page"> <img src="docs/images/windows-phone.png" width="49%" alt="The Windows app's Phone page taking a message">

## Quick start

Linux:

    sudo apt install cmake libgtk-4-dev libadwaita-1-dev libpulse-dev \
        libsndfile1-dev
    cmake -S . -B build && cmake --build build && ./build/linux/dmodem-gui

Windows: use `cmake -S . -B build -A Win32 && cmake --build build --config Release`.
For Windows 95 and later, use `windows/build-watcom.sh` with Open Watcom 2.0.

macOS: first use `cmake -S . -B build -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 && cmake --build build`
(this builds libssh, for SSH). Then use `brew install xcodegen && cd macos && xcodegen && open DModem.xcodeproj`.

The first CMake build downloads libssh and mbedTLS for SSH. This always
occurs on Windows and macOS. On Linux, it occurs if the libssh of the system
is older than 0.11.

The Settings page needs a dmodemd with AT!M (the main branch of D-Modem
since D-Modem/D-Modem#103). The other pages operate with all versions of
D-Modem 1.0.

## Documentation

- [Design](docs/design.md): how the apps are built, what they share, and the Windows 9x notes
- [libdmodemctl](core/include/dmodemctl.h): the API of the library
- [Documentation style](docs/STYLE.md): the rules for ASD-STE100 Simplified Technical English

## The D-Modem suite

| Repository | What it is |
| --- | --- |
| [D-Modem](https://github.com/D-Modem/D-Modem) | The open V.92 softmodem: dmodemd, libdmodem and the Asterisk, FreeSWITCH and ARI modules |
| [app](https://github.com/D-Modem/app) | The D-Modem app for Android and iOS: a dial-up modem on a SIP call |
| [gui](https://github.com/D-Modem/gui) | D-Modem on the desktop: native apps for Windows, macOS and Linux |
| [hardware](https://github.com/D-Modem/hardware) | pico-serial: RP2040 serial adapters and phone-line USB sticks |
| [menu](https://github.com/D-Modem/menu) | modemmenu: the login and menu dial-up callers reach |
| [bbs](https://github.com/D-Modem/bbs) | A bulletin board for telnet, SSH, TN3270 and modem callers |
| [go3270](https://github.com/D-Modem/go3270) | A Go library for 3270 terminal servers, used by bbs |
| [brand](https://github.com/D-Modem/brand) | Logos, colours, type and the naming guide |

## Licence

AGPL-3.0-only ([LICENSE](LICENSE)), or a commercial licence from the D-Modem Authors ([COMMERCIAL-LICENSE.md](COMMERCIAL-LICENSE.md)). Contributions come with a contributor agreement ([CONTRIBUTING.md](CONTRIBUTING.md)).
