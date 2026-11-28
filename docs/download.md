# Download

The [nightly release](https://github.com/D-Modem/gui/releases/tag/nightly)
has the app from the newest build of `main`. A workflow
(`.github/workflows/nightly.yml`) replaces the release one time each day.
When `main` has no new commits, the release does not change.

!!! warning "These builds are not stable releases"
    The files are not signed. Thus Windows SmartScreen and macOS Gatekeeper
    can show a warning before the app starts.

## Select a file

| System | File | What it is |
| --- | --- | --- |
| Windows 95, 98, ME, NT 4.0 and later | [`dmodem-win9x.exe`](https://github.com/D-Modem/gui/releases/download/nightly/dmodem-win9x.exe) | The app with the built-in modem. It needs no C runtime DLL. Windows 95 needs Winsock 2. |
| Windows 98 SE, ME, 2000 and later | [`dmodem-win98.exe`](https://github.com/D-Modem/gui/releases/download/nightly/dmodem-win98.exe) | The app with the built-in modem, on `msvcrt.dll`. |
| Windows 7 and later | [`dmodem-win7.exe`](https://github.com/D-Modem/gui/releases/download/nightly/dmodem-win7.exe) | The app with the built-in modem. |
| Linux, x86_64 (X11) | [`dmodem-win32-linux-x86_64.tar.gz`](https://github.com/D-Modem/gui/releases/download/nightly/dmodem-win32-linux-x86_64.tar.gz) | The app on Linux, with the built-in modem. |
| macOS, arm64 (Apple silicon) | [`dmodem-win32-macos-arm64.tar.gz`](https://github.com/D-Modem/gui/releases/download/nightly/dmodem-win32-macos-arm64.tar.gz) | The app on macOS, with the built-in modem. |
| macOS, x86_64 (Intel Mac) | [`dmodem-win32-macos-x86_64.tar.gz`](https://github.com/D-Modem/gui/releases/download/nightly/dmodem-win32-macos-x86_64.tar.gz) | The app on macOS, with the built-in modem. |
| Mac OS X 10.4 and later, PowerPC and Intel | [`dmodem-win32-macosx10.4.tar.gz`](https://github.com/D-Modem/gui/releases/download/nightly/dmodem-win32-macosx10.4.tar.gz) | The app with the built-in modem. One program for PowerPC (G3, G4, G5) and Intel (i386). It is not tested on a Mac yet. |
| All | [`SHA256SUMS`](https://github.com/D-Modem/gui/releases/download/nightly/SHA256SUMS) | The SHA-256 of each file. |

If a file is not in the release, the newest build did not make it. The
notes of the release list the files that are not there.

## Check a file

1. Download the file and `SHA256SUMS` into the same directory.
2. Open a terminal in this directory.
3. Type `sha256sum --check --ignore-missing SHA256SUMS`.

The result for the file must be `OK`.

## Install on Windows

1. Download the `.exe` file for your version of Windows.
2. Double-click the file. The app does not need an installer.
3. If Windows SmartScreen shows a warning, click **More info**, then
   click **Run anyway**.

## Install on macOS

1. Download `dmodem-win32-macos-arm64.tar.gz` for a Mac with Apple
   silicon, or `dmodem-win32-macos-x86_64.tar.gz` for an Intel Mac.
2. Extract the file. In a terminal, type
   `tar -xzf dmodem-win32-macos-arm64.tar.gz` (or the name of the x86_64
   file).
3. Remove the quarantine attribute. Type
   `xattr -d com.apple.quarantine dmodem-win32`. The app is not notarized.
4. Start the app. Type `./dmodem-win32`.

## Install on Mac OS X 10.4 to 10.6

1. Download `dmodem-win32-macosx10.4.tar.gz`. The file is for PowerPC
   Macs and for Intel Macs.
2. Extract the file. In Terminal, type
   `tar -xzf dmodem-win32-macosx10.4.tar.gz`.
3. Start the app. Type `./dmodem-win32`.

## Install on Linux

1. Install the libraries. On Ubuntu 24.04, type
   `sudo apt install libx11-6 libusb-1.0-0 libasound2t64`.
2. Download `dmodem-win32-linux-x86_64.tar.gz`.
3. Extract the file. Type `tar -xzf dmodem-win32-linux-x86_64.tar.gz`.
4. Start the app. Type `./dmodem-win32`.

## Build the app

To build the app from the source code, refer to [Building](building.md).
