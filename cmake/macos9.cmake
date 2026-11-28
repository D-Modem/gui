# CMake toolchain file for the classic Mac OS on PowerPC (Mac OS 8.6 to
# 9.2). It builds the Windows app (winport/) over the Toolbox backend of
# clang-retro MFC, on Linux.
#
#     cmake -S . -B build/macos9 -DCMAKE_TOOLCHAIN_FILE=cmake/macos9.cmake \
#         -DMACOS9_LLVM=/opt/llvm -DMACOS9_SDK=/path/to/mac-sdk \
#         -DCLANG_RETRO_MFC=/path/to/mfc -DDMODEM_GUI_WINPORT=ON
#
# You need these:
#   MACOS9_LLVM      clang of clang-retro/llvm (the llvm-prebuilt release)
#   MACOS9_SDK       a clang-retro/mac-sdk checkout, with its runtime, C and
#                    C++ libraries in lib/ (runtime/build.sh,
#                    runtime/build-libc.sh, runtime/build-libstdcxx.sh)
#   CLANG_RETRO_MFC  a clang-retro/mfc checkout (port/compat/macos)
# The environment variables of the same names also set them.
#
# CMake builds the static libraries. tools/macapp.py of mac-sdk links each
# program into a PowerPC application (a MacBinary II file, .bin).
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")
set(CMAKE_SYSTEM_NAME MacOS9)
set(CMAKE_SYSTEM_VERSION 9.2)
set(CMAKE_SYSTEM_PROCESSOR powerpc)

foreach(_v MACOS9_LLVM MACOS9_SDK CLANG_RETRO_MFC)
  if(NOT ${_v} AND DEFINED ENV{${_v}})
    set(${_v} "$ENV{${_v}}")
  endif()
  if(NOT ${_v})
    message(FATAL_ERROR "Set ${_v} (cmake/macos9.cmake)")
  endif()
  set(${_v} "${${_v}}" CACHE PATH "" FORCE)
  # (the builds that D-Modem configures with this file see them too)
  set(ENV{${_v}} "${${_v}}")
endforeach()
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES MACOS9_LLVM MACOS9_SDK CLANG_RETRO_MFC)

set(MACOS9_TARGET powerpc-apple-macos9)
set(_lib "${MACOS9_SDK}/lib")
# The headers: GCC's C++ library of mac-sdk, port/compat/macos of
# clang-retro MFC (the POSIX declarations that picolibc does not have),
# the C library, then the Toolbox. These are mandatory arguments of the
# compilers, with the target: a -DCMAKE_C_FLAGS of a project or an
# ExternalProject keeps them.
set(_common -isystem ${CLANG_RETRO_MFC}/port/compat/macos -isystem ${_lib}/include
  -I${MACOS9_SDK}/include -DMAC_LIBC -D_GNU_SOURCE
  -femulated-tls -ffunction-sections -fdata-sections)
set(CMAKE_C_COMPILER "${MACOS9_LLVM}/bin/clang" "--target=${MACOS9_TARGET}" ${_common})
set(CMAKE_CXX_COMPILER "${MACOS9_LLVM}/bin/clang++" "--target=${MACOS9_TARGET}"
  -nostdinc++ -isystem ${_lib}/include/c++/15.2.0/${MACOS9_TARGET}
  -isystem ${_lib}/include/c++/15.2.0 ${_common} -fexceptions -frtti)
set(CMAKE_AR "${MACOS9_LLVM}/bin/llvm-ar")
set(CMAKE_RANLIB "${MACOS9_LLVM}/bin/llvm-ranlib")

# The tests of CMake and of the projects compile and do not link: the
# linker is macapp.py, which makes a Mac application
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# A program: macapp.py links its objects and libraries with the C and
# C++ libraries of mac-sdk (MACOS9_APP_SIZE: the partition in KB)
set(MACOS9_APP_SIZE 16384 CACHE STRING "The partition of a program, in KB")
find_program(MACOS9_PYTHON NAMES python3 python REQUIRED)
# (macapp.py adds the C++ library only for C++ sources, not for objects:
# a C++ program names libstdc++.a and libunwind.a, which throws the
# exceptions, itself)
set(_macapp "${CMAKE_COMMAND} -E env CLANG=${MACOS9_LLVM}/bin/clang ${MACOS9_PYTHON} ${MACOS9_SDK}/tools/macapp.py --cpu ppc --size ${MACOS9_APP_SIZE} --name <TARGET_NAME> -o <TARGET> -- <OBJECTS> <LINK_LIBRARIES>")
set(CMAKE_C_LINK_EXECUTABLE "${_macapp}")
set(CMAKE_CXX_LINK_EXECUTABLE "${_macapp} ${_lib}/ppc/libstdc++.a ${_lib}/ppc/libunwind.a")

set(CMAKE_FIND_ROOT_PATH "${_lib}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
