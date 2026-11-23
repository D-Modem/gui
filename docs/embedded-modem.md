# The built-in modem

The Linux app, the macOS app and the Windows app (Windows 98 SE and
later) have the D-Modem modem in them. Thus, these apps do not need a
separate `dmodemd`. The Windows 95 build still uses `dmodemd`.

## What it does

The app uses `libdmodem_embed`, the library of `dmodemd` in the D-Modem
repository ([docs/embed.md](https://github.com/D-Modem/D-Modem/blob/main/docs/embed.md)).

1. When you click **Start**, the app starts the modem in a thread of the
   app.
2. The library gives the app a socket. This socket is the DTE of the modem.
   The app uses the socket as it uses the pty of `dmodemd`.
3. When you click **Stop**, the app hangs up the call and stops the thread.

The built-in modem has these lines: a SIP account (or no account), a USB
modem, and a softmodem codec (ALSA, Linux only). For "A modem that already runs", the
app opens the device and starts no modem.

## Differences from dmodemd

- There is no pty and no `/dev/ttyDM0` link. Other programs cannot use the
  built-in modem. To share the modem with other programs, start `dmodemd`.
- The app does not need root (Linux), a pty (macOS) or a firewall
  exception for a TCP port (Windows).
- A USB modem needs a udev rule that lets the user open the device.
- A softmodem codec needs the user in the `audio` group.
- The modem keeps the measurements of the line in
  `~/.local/share/dmodem-gui/dmodem0.data` (Linux),
  `~/Library/Application Support/D-Modem/dmodem0.data` (macOS), or
  `%APPDATA%\D-Modem\dmodem0.data` (Windows).
- The **Log** page shows the log of the modem.

## How to use dmodemd

On Linux:

1. Open the line settings (the gear button on the Modem page).
2. In **Advanced**, set **Built-in modem** to off.
3. Optional: set the path of `dmodemd`.

On macOS:

1. Open the line settings (**D-Modem > Settings**).
2. In **The modem**, set **Built-in modem** to off.
3. Optional: set the path of `dmodemd`.

On Windows, the Line Settings dialog does not have this setting yet. Close
the app, and add this line to the `[line]` section of
`%APPDATA%\D-Modem\dmodem.ini`:

    builtin=0

The app then starts `dmodemd` as before.

## How to build it

The D-Modem source is a git submodule in `external/D-Modem`. To get it:

    git submodule update --init

If the submodule is there, CMake builds the built-in modem
(`DMODEM_GUI_EMBED`, on by default) for the Linux app, the macOS app, and
the Windows app with the clang-retro configurations `win98`, `winme`, `win7` and later.
It is off for `win32-picolibc` (the Windows 95 build) and for the Windows
app on Linux and macOS (`DMODEM_GUI_WINPORT`). To build the app with no built-in
modem, use `-DDMODEM_GUI_EMBED=OFF`. For a USB line and an ALSA line, also
install `libusb-1.0-0-dev` and `libasound2-dev`.

The first build downloads libre and Mbed TLS for the SIP line of the modem.

The macOS app builds with Xcode, not with CMake. CMake builds the
libraries of the built-in modem, and writes `build/macos-embed.xcconfig`.
This file gives Xcode the header path, the libraries and the Swift
condition `DMODEM_GUI_EMBED`. The Xcode project (`macos/project.yml`)
includes this file. Thus, do the CMake build before `xcodegen`. Refer to
[Building](building.md).

## Licence

D-Modem and the GUI are both under the AGPL-3.0-only, or a commercial
licence from the D-Modem Authors. Thus the two combine under each licence.
The built-in modem links libre (BSD-3-Clause), Mbed TLS (Apache-2.0), and
on Linux the libusb and ALSA libraries of the system (LGPL-2.1). On
Windows and macOS, the build links libusb (LGPL-2.1) statically, as
`dmodemd` does.

## Status

| App | Status |
| --- | --- |
| Linux | Done. Tested: a call from the Dial button to another D-Modem connects at 62666 bit/s (V.91), and **Stop** hangs up. |
| Windows 98 SE and later | Done (`win98` build). It builds, and its imports agree with Windows 98 SE. It is not tested on Windows or Wine yet. |
| Windows 7 and later | Done (`win7` build). It builds. It is not tested on Windows or Wine yet. |
| Windows 95 and NT 4.0 | Not possible. Refer to "Windows 95" below. |
| macOS | Done. CI builds it and checks that the library is in the app. It is not tested on a Mac yet. |

### Windows 95

The Windows 95 build of the app uses the clang-retro configuration
`win32-picolibc`. It has a C library (picolibc) but no C++ library. The
modem is in C++17 and needs the `win98` configuration (msvcrt and libc++).
One program cannot use the two C libraries. Thus, the app with the
built-in modem needs Windows 98 SE or later. The Windows 95 build
continues to use `dmodemd` on TCP, or a COM port. The `win98` build also
does not operate on Windows NT 4.0: its libraries use `_vsnprintf_s` of
msvcrt.dll, which NT 4.0 does not have.

On Windows, the modem gives the app a socket on the loopback interface
(`127.0.0.1`), because Windows does not have `socketpair()`.
