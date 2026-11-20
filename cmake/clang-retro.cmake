# CMake toolchain file for clang-retro (https://clang-retro.github.io/):
# the Windows app for Windows 95 and later, built on Linux or Windows.
#
#     cmake -S . -B build/retro -DCLANG_DOS=/opt/clang-dos \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/clang-retro.cmake
#
# CLANG_DOS is the unpacked clang-dos toolchain package (the environment
# variable CLANG_DOS also sets it).  CLANG_RETRO_CONFIG is the --config
# file of the target (default win32-picolibc: Windows 95, 98, ME, NT 4.0
# and later, with no C runtime DLL).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 4.0)
set(CMAKE_SYSTEM_PROCESSOR i386)

if(NOT CLANG_DOS AND DEFINED ENV{CLANG_DOS})
  set(CLANG_DOS "$ENV{CLANG_DOS}")
endif()
if(NOT CLANG_DOS)
  message(FATAL_ERROR "Set CLANG_DOS to the clang-dos toolchain package")
endif()
set(CLANG_DOS "${CLANG_DOS}" CACHE PATH "The clang-dos toolchain package")
set(CLANG_RETRO_CONFIG win32-picolibc CACHE STRING "The clang-retro --config file (without .cfg)")
# (try_compile projects see these two)
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CLANG_DOS CLANG_RETRO_CONFIG)

if(CMAKE_HOST_WIN32)
  set(_exe .exe)
else()
  set(_exe "")
endif()
# (the --config file is a mandatory argument of the compilers, not a flag,
# so that a -DCMAKE_C_FLAGS of a project, or of an ExternalProject, keeps it)
set(_cfg "--config=${CLANG_DOS}/bin/${CLANG_RETRO_CONFIG}.cfg")
set(CMAKE_C_COMPILER "${CLANG_DOS}/bin/clang${_exe}" "${_cfg}")
set(CMAKE_CXX_COMPILER "${CLANG_DOS}/bin/clang++${_exe}" "${_cfg}")
set(CMAKE_RC_COMPILER "${CLANG_DOS}/bin/llvm-windres${_exe}")
set(CMAKE_AR "${CLANG_DOS}/bin/llvm-ar${_exe}")
set(CMAKE_RANLIB "${CLANG_DOS}/bin/llvm-ranlib${_exe}")
# windres runs the preprocessor of clang for the target
set(CMAKE_RC_FLAGS_INIT "--target=pe-i386 --preprocessor-arg=${_cfg}")

list(APPEND CMAKE_FIND_ROOT_PATH "${CLANG_DOS}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
