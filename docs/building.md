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

## Mac OS X 10.4 and later (PowerPC and Intel)

The app for Mac OS X 10.4 (Tiger) and later builds on Linux. It uses the
Mac OS X 10.0-10.5 port of clang-retro MFC and the toolchain file
`cmake/macosx-legacy.cmake`. You need these items:

- clang of [clang-retro/llvm](https://github.com/clang-retro/llvm) (its
  `llvm-prebuilt` release). The clang of the clang-dos package can be too
  old: it must have emulated TLS for Mac OS X 10.4.
- The `macosx10.4` sysroot of [clang-retro/dos-sdk](https://github.com/clang-retro/dos-sdk):
  `sh packaging/get-toolchain.sh -s macosx10.4 DIR`. It has libc++ and
  the builtins, and the script also gets Apple's `MacOSX10.4u.sdk`.
  Apple's license applies to the SDK.
- Apple's `ld64` and `ar` for PowerPC: `PREFIX=DIR sh scripts/build-cctools-ppc.sh`
  in dos-sdk. This `ld64` also links for i386.

Build the app one time for each processor (`powerpc` and `i386`):

    export MACOSX_LEGACY_LLVM=/path/to/llvm MACOSX_LEGACY_SYSROOT=/path/to/macosx10.4 \
        MACOSX_LEGACY_CCTOOLS=/path/to/cctools-ppc
    cmake -S . -B build/powerpc -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/macosx-legacy.cmake \
        -DMACOSX_LEGACY_ARCH=powerpc -DCLANG_RETRO_MFC=/path/to/mfc -DDMODEM_GUI_WINPORT=ON
    cmake --build build/powerpc --target dmodem-win32

Then join the two programs into one fat program:

    $MACOSX_LEGACY_CCTOOLS/bin/powerpc-apple-darwin8-lipo -create \
        build/powerpc/winport/dmodem-win32 build/i386/winport/dmodem-win32 -output dmodem-win32

`cmake/macosx-legacy/` has the fixes for headers of the 10.4 SDK
(`time.h`, `signal.h` and `net/if.h`). The built-in modem needs D-Modem
with the changes for 10.4 to 10.6 (`docs/macos-details.md` in D-Modem).

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
