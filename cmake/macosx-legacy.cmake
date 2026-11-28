# CMake toolchain file for Mac OS X 10.4 (Tiger) and later, on 32-bit
# PowerPC or 32-bit Intel. It builds the Windows app (winport/) over the
# Mac OS X 10.0-10.5 port of clang-retro MFC, on Linux.
#
#     cmake -S . -B build/ppc -DCMAKE_TOOLCHAIN_FILE=cmake/macosx-legacy.cmake \
#         -DMACOSX_LEGACY_ARCH=powerpc -DMACOSX_LEGACY_LLVM=/opt/llvm \
#         -DMACOSX_LEGACY_SYSROOT=/opt/clang-dos/macosx10.4 \
#         -DMACOSX_LEGACY_CCTOOLS=/opt/cctools-ppc \
#         -DCLANG_RETRO_MFC=/path/to/mfc -DDMODEM_GUI_WINPORT=ON
#
# You need these:
#   MACOSX_LEGACY_ARCH     powerpc or i386
#   MACOSX_LEGACY_LLVM     clang of clang-retro/llvm (the llvm-prebuilt
#                          release), with emulated TLS for Mac OS X 10.4
#   MACOSX_LEGACY_SYSROOT  the macosx10.4 sysroot of clang-retro/dos-sdk
#                          (packaging/get-toolchain.sh -s macosx10.4): libc++,
#                          and MacOSX10.4u.sdk in it
#   MACOSX_LEGACY_CCTOOLS  Apple's ld64 and ar for PowerPC
#                          (dos-sdk's scripts/build-cctools-ppc.sh)
# The environment variables of the same names also set them.
set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_VERSION 8.0)

foreach(_v ARCH LLVM SYSROOT CCTOOLS)
  if(NOT MACOSX_LEGACY_${_v} AND DEFINED ENV{MACOSX_LEGACY_${_v}})
    set(MACOSX_LEGACY_${_v} "$ENV{MACOSX_LEGACY_${_v}}")
  endif()
  if(NOT MACOSX_LEGACY_${_v})
    message(FATAL_ERROR "Set MACOSX_LEGACY_${_v} (cmake/macosx-legacy.cmake)")
  endif()
  set(MACOSX_LEGACY_${_v} "${MACOSX_LEGACY_${_v}}" CACHE STRING "" FORCE)
  # (the builds that D-Modem configures with this file see them too)
  set(ENV{MACOSX_LEGACY_${_v}} "${MACOSX_LEGACY_${_v}}")
endforeach()
# (try_compile projects see these)
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
  MACOSX_LEGACY_ARCH MACOSX_LEGACY_LLVM MACOSX_LEGACY_SYSROOT MACOSX_LEGACY_CCTOOLS)

set(CMAKE_SYSTEM_PROCESSOR ${MACOSX_LEGACY_ARCH})
set(MACOSX_LEGACY_TARGET "${MACOSX_LEGACY_ARCH}-apple-macosx10.4")
set(MACOSX_LEGACY_SDK "${MACOSX_LEGACY_SYSROOT}/MacOSX10.4u.sdk")
set(_cct "${MACOSX_LEGACY_CCTOOLS}/bin/powerpc-apple-darwin8-")

set(CMAKE_OSX_SYSROOT "${MACOSX_LEGACY_SDK}")
set(CMAKE_OSX_DEPLOYMENT_TARGET 10.4)
set(CMAKE_OSX_ARCHITECTURES "")
# ld64 cannot read the symbol table that llvm-ar writes for PowerPC
set(CMAKE_AR "${_cct}ar")
set(CMAKE_RANLIB "${_cct}ranlib")
set(CMAKE_LIPO "${_cct}lipo")
set(CMAKE_INSTALL_NAME_TOOL "${_cct}install_name_tool")

# The C library functions that Mac OS X 10.4 does not have, such as
# clock_gettime: port/compat/macosx of clang-retro MFC (CLANG_RETRO_MFC),
# and cmake/macosx-legacy (the fixes of headers of the 10.4 SDK).
# libwinport.a has their code. _DARWIN_C_SOURCE: gmtime_r and the other
# POSIX functions also with -std=c99. And libc++ of the sysroot, with
# threads.
if(NOT CLANG_RETRO_MFC AND DEFINED ENV{CLANG_RETRO_MFC})
  set(CLANG_RETRO_MFC "$ENV{CLANG_RETRO_MFC}")
endif()
if(NOT CLANG_RETRO_MFC)
  message(FATAL_ERROR "Set CLANG_RETRO_MFC (cmake/macosx-legacy.cmake)")
endif()
set(ENV{CLANG_RETRO_MFC} "${CLANG_RETRO_MFC}")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CLANG_RETRO_MFC)
# These are mandatory arguments of the compilers, with the target, not
# flags: a -DCMAKE_C_FLAGS of a project or an ExternalProject keeps them.
set(_compat -isystem ${CMAKE_CURRENT_LIST_DIR}/macosx-legacy
  -isystem ${CLANG_RETRO_MFC}/port/compat/macosx
  -DWINPORT_MACOSX_COMPAT -D_DARWIN_C_SOURCE)
set(_cxx -nostdinc++ -isystem ${MACOSX_LEGACY_SYSROOT}/include/c++/v1
  -D__STDC_FORMAT_MACROS)
set(CMAKE_C_COMPILER "${MACOSX_LEGACY_LLVM}/bin/clang"
  "--target=${MACOSX_LEGACY_TARGET}" ${_compat})
set(CMAKE_CXX_COMPILER "${MACOSX_LEGACY_LLVM}/bin/clang++"
  "--target=${MACOSX_LEGACY_TARGET}" ${_cxx} ${_compat})
set(CMAKE_OBJCXX_COMPILER ${CMAKE_CXX_COMPILER})
# The builtins of the sysroot have emulated TLS and the atomic library
# functions (__atomic_fetch_add_8 and others), which libc++ and libre need
if(MACOSX_LEGACY_ARCH STREQUAL "powerpc")
  set(_rt "${MACOSX_LEGACY_SYSROOT}/lib/darwin/libclang_rt.ppc_osx.a")
else()
  set(_rt "${MACOSX_LEGACY_SYSROOT}/lib/darwin/libclang_rt.osx.a")
endif()
set(_ld "-fuse-ld=${_cct}ld -nostdlib++ -L${MACOSX_LEGACY_SYSROOT}/lib -lc++ -lc++abi -lunwind ${_rt}")
# (a program exports no symbols of libc++: on i386, ld64 stops at a
# pointer difference to a global weak symbol, such as std::to_string's
# to __itoa::__pow10_32 in the libc++ of the sysroot, when another object
# also has that symbol)
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_ld} -Wl,-unexported_symbol,__ZNSt3__1*")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_ld}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_ld}")

list(APPEND CMAKE_FIND_ROOT_PATH "${MACOSX_LEGACY_SDK}" "${MACOSX_LEGACY_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
