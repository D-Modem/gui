# Building

Linux:

    sudo apt install cmake libgtk-4-dev libadwaita-1-dev libpulse-dev \
        libsndfile1-dev
    cmake -S . -B build && cmake --build build && ./build/linux/dmodem-gui

Windows: we build the Windows app with clang-retro. Get its toolchain
package (`clang-dos-linux-x86_64.tar.xz` or `clang-dos-windows-x86_64.zip`,
from the [nightly release](https://github.com/clang-retro/dos-sdk/releases/tag/nightly)).
Then set `CLANG_DOS` to the directory of the package:

    # Windows 95, 98, ME, NT 4.0 and later (no C runtime DLL)
    cmake -S . -B build/win9x -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCLANG_DOS=/opt/clang-dos -DCMAKE_TOOLCHAIN_FILE=cmake/clang-retro.cmake
    cmake --build build/win9x

    # Windows 7 and later
    cmake -S . -B build/win7 -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCLANG_DOS=/opt/clang-dos -DCMAKE_TOOLCHAIN_FILE=cmake/clang-retro.cmake \
        -DCLANG_RETRO_CONFIG=win7
    cmake --build build/win7

The app is in `windows/dmodem.exe` of the build directory. MSVC
(`cmake -S . -B build -A Win32`) and MinGW also build the same code.

macOS: first use `cmake -S . -B build -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 && cmake --build build`
(this builds libssh, for SSH). Then use `brew install xcodegen && cd macos && xcodegen && open DModem.xcodeproj`.

The Windows app on Linux or macOS: get [clang-retro MFC](https://github.com/clang-retro/mfc),
clang and `llvm-rc` (on macOS, `brew install llvm`). Then:

    cmake -S . -B build -DDMODEM_GUI_WINPORT=ON -DCLANG_RETRO_MFC=/path/to/mfc \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
    cmake --build build --target dmodem-win32 && ./build/winport/dmodem-win32

The first CMake build downloads libssh and mbedTLS for SSH. This always
occurs on Windows and macOS. Use `-DDMODEM_GUI_SSH=OFF` for a build with
no SSH. On Linux, it occurs if the libssh of the system
is older than 0.11.

The Settings page needs a dmodemd with AT!M (the main branch of D-Modem
since D-Modem/D-Modem#103). The other pages operate with all versions of
D-Modem 1.0.
