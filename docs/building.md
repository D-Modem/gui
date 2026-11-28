# Building

The app is one Win32 app (`windows/`). It builds for Windows, and for
Linux and macOS over the portable Win32 layer of clang-retro MFC
(`winport/`). The submodule `external/D-Modem` gives the built-in modem.
Refer to [The built-in modem](embedded-modem.md).

## Windows

We build the app for Windows with clang-retro. Get its toolchain
package (`clang-dos-linux-x86_64.tar.xz` or `clang-dos-windows-x86_64.zip`,
from the [nightly release](https://github.com/clang-retro/dos-sdk/releases/tag/nightly)).
Then set `CLANG_DOS` to the directory of the package. Each build needs the
submodule (`git submodule update --init`) for the built-in modem:

    # Windows 95, 98, ME, NT 4.0 and later (no C runtime DLL), with the built-in modem
    cmake -S . -B build/win9x -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCLANG_DOS=/opt/clang-dos -DCMAKE_TOOLCHAIN_FILE=cmake/clang-retro.cmake
    cmake --build build/win9x

    # Windows 98 SE, ME, 2000 and later, with the built-in modem
    cmake -S . -B build/win98 -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCLANG_DOS=/opt/clang-dos -DCMAKE_TOOLCHAIN_FILE=cmake/clang-retro.cmake \
        -DCLANG_RETRO_CONFIG=win98
    cmake --build build/win98

    # Windows 7 and later, with the built-in modem
    cmake -S . -B build/win7 -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCLANG_DOS=/opt/clang-dos -DCMAKE_TOOLCHAIN_FILE=cmake/clang-retro.cmake \
        -DCLANG_RETRO_CONFIG=win7
    cmake --build build/win7

The app is in `windows/dmodem.exe` of the build directory. MSVC
(`cmake -S . -B build -A Win32`) and MinGW also build the same code.

## Linux and macOS

Get [clang-retro MFC](https://github.com/clang-retro/mfc),
clang and `llvm-rc` (on macOS, `brew install llvm`). For the built-in
modem, get the D-Modem submodule (`git submodule update --init`). On
Linux, also install `libusb-1.0-0-dev` and `libasound2-dev`. Then:

    cmake -S . -B build -DDMODEM_GUI_WINPORT=ON -DCLANG_RETRO_MFC=/path/to/mfc \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
    cmake --build build --target dmodem-win32 && ./build/winport/dmodem-win32

On Linux, `cmake --install build` installs the app, its desktop entry,
its icon and the firmware of the MultiTech MT9234MU (`firmware/`).

On macOS, the app is for the architecture of the Mac. To build the app
for an Intel Mac on a Mac with Apple silicon, add
`-DCMAKE_OSX_ARCHITECTURES=x86_64` to the first command. Set one
architecture only. The app cannot be a universal binary.

## The library only

With no `DMODEM_GUI_WINPORT`, a build on Linux or macOS makes only
libdmodemctl, libdmodemssh and their tests. You do not need clang-retro
MFC for this build:

    cmake -S . -B build && cmake --build build && ctest --test-dir build

## Notes

The first CMake build downloads libssh and mbedTLS for SSH. This always
occurs on Windows and macOS. Use `-DDMODEM_GUI_SSH=OFF` for a build with
no SSH. On Linux, it occurs if the libssh of the system
is older than 0.11.

The Settings page needs a dmodemd with AT!M (the main branch of D-Modem
since D-Modem/D-Modem#103). The other pages operate with all versions of
D-Modem 1.0.

## Updates from D-Modem

The workflow `.github/workflows/d-modem-bump.yml` keeps the submodule
`external/D-Modem` on the main branch of D-Modem. It starts when the main
branch of D-Modem changes, one time each day, or when you start it
manually. It opens a pull request that moves the submodule. Build
(`.github/workflows/build.yml`) builds and tests the app in that pull
request. The workflow merges the pull request when all checks pass. Then
Build runs on `main`, and the next nightly release has the new app. If a
check fails, the pull request stays open for a person. The workflow needs
a `DMODEM_BUMP_TOKEN` secret. The header of the workflow gives the
necessary permissions.
