# Download

The [nightly release](https://github.com/D-Modem/gui/releases/tag/nightly)
has the apps from the newest build of `main`. A workflow
(`.github/workflows/nightly.yml`) replaces the release one time each day.
When `main` has no new commits, the release does not change.

!!! warning "These builds are not stable releases"
    The files are not signed. Thus Windows SmartScreen and macOS Gatekeeper
    can show a warning before the app starts.

## Select a file

| System | File | What it is |
| --- | --- | --- |
| Windows 95, 98, ME, NT 4.0 and later | [`dmodem-win9x.exe`](https://github.com/D-Modem/gui/releases/download/nightly/dmodem-win9x.exe) | The Windows app with the built-in modem. It needs no C runtime DLL. Windows 95 needs Winsock 2. |
| Windows 98 SE, ME, 2000 and later | [`dmodem-win98.exe`](https://github.com/D-Modem/gui/releases/download/nightly/dmodem-win98.exe) | The Windows app with the built-in modem, on `msvcrt.dll`. |
| Windows 7 and later | [`dmodem-win7.exe`](https://github.com/D-Modem/gui/releases/download/nightly/dmodem-win7.exe) | The Windows app with the built-in modem. |
| macOS 13 and later | [`D-Modem-macos.zip`](https://github.com/D-Modem/gui/releases/download/nightly/D-Modem-macos.zip) | The macOS app (SwiftUI), for arm64 and x86_64. |
| Linux, x86_64 | [`dmodem-gui-linux-x86_64.tar.gz`](https://github.com/D-Modem/gui/releases/download/nightly/dmodem-gui-linux-x86_64.tar.gz) | The Linux app (GTK 4). |
| Linux, x86_64 (X11) | [`dmodem-win32-linux-x86_64.tar.gz`](https://github.com/D-Modem/gui/releases/download/nightly/dmodem-win32-linux-x86_64.tar.gz) | The Windows app on Linux. |
| macOS, arm64 | [`dmodem-win32-macos-arm64.tar.gz`](https://github.com/D-Modem/gui/releases/download/nightly/dmodem-win32-macos-arm64.tar.gz) | The Windows app on macOS. |
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

1. Download `D-Modem-macos.zip`.
2. Double-click the file to extract the app.
3. Move the app to the **Applications** folder.
4. Right-click the app, then select **Open**. The app is not notarized.
   Thus macOS shows a warning the first time.

## Install on Linux

The Linux app needs GTK 4, libadwaita, PulseAudio and libsndfile.

1. Install the libraries. On Ubuntu 24.04, type
   `sudo apt install libgtk-4-1 libadwaita-1-0 libpulse0 libsndfile1`.
2. Download `dmodem-gui-linux-x86_64.tar.gz`.
3. Extract the file into `/`. Type
   `sudo tar -xzf dmodem-gui-linux-x86_64.tar.gz -C /`.
4. Start the app from the menu of applications, or type `dmodem-gui`.

## Build the app

To build the app from the source code, refer to [Building](building.md).
