# The built-in modem

The app has the D-Modem modem in it, on Windows (Windows 95 and later),
on Linux and on macOS. Thus, the app does not need a separate `dmodemd`.

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
- The app does not need root or a pty (Linux and macOS), or a firewall
  exception for a TCP port (Windows).
- A USB modem needs a udev rule that lets the user open the device.
- A softmodem codec needs the user in the `audio` group.
- The modem keeps the measurements of the line in
  `~/.config/D-Modem/dmodem0.data` (Linux),
  `~/Library/Application Support/D-Modem/dmodem0.data` (macOS), or
  `%APPDATA%\D-Modem\dmodem0.data` (Windows).
- The **Log** page shows the log of the modem.

## How to use dmodemd

The Line Settings dialog does not have this setting yet. Close the app,
and add this line to the `[line]` section of the settings file:

    builtin=0

The settings file is `%APPDATA%\D-Modem\dmodem.ini` (Windows),
`~/.config/D-Modem/dmodem.ini` (Linux) or
`~/Library/Application Support/D-Modem/dmodem.ini` (macOS).

The app then starts `dmodemd` as before.

## How to build it

The D-Modem source is a git submodule in `external/D-Modem`. To get it:

    git submodule update --init

If the submodule is there, CMake builds the built-in modem
(`DMODEM_GUI_EMBED`, on by default) for the app on Windows with the
clang-retro configurations `win32-picolibc` (the Windows 95 build),
`win98`, `winme`, `win7` and later, and for the app on Linux and macOS
(`DMODEM_GUI_WINPORT`). To build the app with no built-in
modem, use `-DDMODEM_GUI_EMBED=OFF`. For a USB line and an ALSA line, also
install `libusb-1.0-0-dev` and `libasound2-dev`.

The first build downloads libre and Mbed TLS for the SIP line of the modem.

## Licence

D-Modem and the GUI are both under the AGPL-3.0-only, or a commercial
licence from the D-Modem Authors. Thus the two combine under each licence.
The built-in modem links libre (BSD-3-Clause), Mbed TLS (Apache-2.0), and
on Linux the libusb and ALSA libraries of the system (LGPL-2.1). On
Windows and macOS, the build links libusb (LGPL-2.1) statically, as
`dmodemd` does.

## Status

| System | Status |
| --- | --- |
| Windows 98 SE and later | Done (`win98` build). It builds, and its imports agree with Windows 98 SE. It is not tested on Windows or Wine yet. |
| Windows 7 and later | Done (`win7` build). It builds. It is not tested on Windows or Wine yet. |
| Windows 95, NT 4.0 and later | Done (`win32-picolibc` build). It builds, and its imports agree with Windows 95 and NT 4.0. It is not tested on Windows or Wine yet. Refer to "Windows 95" below. |
| Linux | Done. CI builds it, checks that the library is in the app, and starts the built-in modem in Xvfb (`winport/test/smoke.sh`). |
| macOS | Done (arm64 and x86_64). CI builds it for each architecture, and checks that the library is in the app. It is not tested on a Mac yet. |

### Windows 95

The Windows 95 build of the app uses the clang-retro configuration
`win32-picolibc`. Its C library is picolibc and its C++ library is libc++.
Both are linked into the app, so the app does not need `msvcrt.dll`. The
modem (C++17) builds with this configuration. D-Modem changes some names
of libre and of its own Windows code for picolibc (refer to
"picolibc" in D-Modem's `docs/windows-details.md`).

- On Windows 95, the app needs Winsock 2 (`ws2_32.dll`). Install the
  Windows Socket 2 Update of Microsoft. The SSH page needs it too.
- The modem does not need `iphlpapi.dll`, which Windows 95 does not have.
  libre finds `GetAdaptersInfo()` at run time.
- The `win98` build does not operate on Windows NT 4.0: its libraries use
  `_vsnprintf_s` of msvcrt.dll, which NT 4.0 does not have. Use the
  `win32-picolibc` build on NT 4.0.

On Windows, the modem gives the app a socket on the loopback interface
(`127.0.0.1`), because Windows does not have `socketpair()`.
