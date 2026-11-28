# D-Modem's desktop app: the shared C core (libdmodemctl), and the
# Win32 app (windows/). On Windows, it builds with clang-retro
# (cmake/clang-retro.cmake), MinGW or MSVC. With DMODEM_GUI_WINPORT, the
# same app builds for Linux and macOS, over clang-retro MFC's portable
# Win32 layer (winport/). On Linux and macOS with no DMODEM_GUI_WINPORT,
# only the core, libdmodemssh and their tests build.
# With DMODEM_GUI_EMBED, the app (Windows 95 and later, and the app on
# Linux and macOS) has the modem in it (libdmodem_embed from the D-Modem
# submodule, external/D-Modem): it needs no dmodemd.
cmake_minimum_required(VERSION 3.16)
project(dmodem-gui VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(DMODEM_GUI_SSH "Build libdmodemssh (libssh) for the Remote page" ON)
option(DMODEM_GUI_WINPORT "Build the Windows app for Linux or macOS (winport/)" OFF)
set(CLANG_RETRO_MFC "" CACHE PATH "A clang-retro MFC checkout, for DMODEM_GUI_WINPORT")

# the modem built in (docs/embedded-modem.md): the app on Windows, and
# the app on Linux and macOS (winport/), when the submodule is there
# (git submodule update --init)
set(DMODEM_GUI_DMODEM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/D-Modem
  CACHE PATH "The D-Modem tree, for DMODEM_GUI_EMBED")
# (all the Windows builds: clang-retro's win32-picolibc, the Windows 95
# build, has libc++ for the modem's C++17, as win98 and win7 do)
set(_embed_default OFF)
if(EXISTS ${DMODEM_GUI_DMODEM_DIR}/CMakeLists.txt)
  if(WIN32 OR DMODEM_GUI_WINPORT)
    set(_embed_default ON)
  endif()
endif()
option(DMODEM_GUI_EMBED "Build the modem into the app (libdmodem_embed)"
  ${_embed_default})
if(DMODEM_GUI_EMBED AND (WIN32 OR DMODEM_GUI_WINPORT))
  if(NOT EXISTS ${DMODEM_GUI_DMODEM_DIR}/CMakeLists.txt)
    message(FATAL_ERROR "DMODEM_GUI_EMBED: no D-Modem in "
      "${DMODEM_GUI_DMODEM_DIR} (git submodule update --init)")
  endif()
  enable_language(CXX)
  # (D-Modem's library and its SIP alone: no programs, modules or tests)
  set(DMODEM_DMODEMD ON CACHE BOOL "" FORCE)
  set(DMODEM_EMBED ON CACHE BOOL "" FORCE)
  set(DMODEM_TESTS OFF CACHE BOOL "" FORCE)
  set(DMODEM_ASTERISK OFF CACHE BOOL "" FORCE)
  set(DMODEM_FREESWITCH OFF CACHE BOOL "" FORCE)
  set(DMODEM_ARI OFF CACHE BOOL "" FORCE)
  # (clang-retro: D-Modem's own toolchain file has its sources take
  # dmodem_retro.h, the clock_gettime() that the sysroots lack; here
  # its C++ sources alone, as there)
  get_directory_property(_opts COMPILE_OPTIONS)
  if(CLANG_DOS)
    # (the oldest Windows of the target, as D-Modem's toolchain file
    # has it: the .cfg's _WIN32_WINNT, else its triple's
    # windows<major>.<minor>, 9x and NT 4.0 all 0x0400)
    if(NOT DMODEM_WINNT)
      file(STRINGS ${CLANG_DOS}/bin/${CLANG_RETRO_CONFIG}.cfg _cfg_lines)
      foreach(_l ${_cfg_lines})
        if(_l MATCHES "^-D_WIN32_WINNT=(.*)$")
          set(DMODEM_WINNT ${CMAKE_MATCH_1})
        elseif(NOT DMODEM_WINNT AND _l MATCHES "windows([0-9]+)\\.([0-9]+)")
          if(CMAKE_MATCH_1 EQUAL 4)
            set(DMODEM_WINNT 0x0400)
          else()
            math(EXPR DMODEM_WINNT "${CMAKE_MATCH_1} * 256 + ${CMAKE_MATCH_2}"
              OUTPUT_FORMAT HEXADECIMAL)
          endif()
        endif()
      endforeach()
      set(DMODEM_WINNT ${DMODEM_WINNT} CACHE STRING
        "_WIN32_WINNT: the oldest Windows for the modem built in")
    endif()
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-include>"
      "$<$<COMPILE_LANGUAGE:CXX>:${DMODEM_GUI_DMODEM_DIR}/platforms/windows/clang-retro/dmodem_retro.h>")
    set(ENV{CLANG_DOS} ${CLANG_DOS})
  endif()
  add_subdirectory(${DMODEM_GUI_DMODEM_DIR} d-modem EXCLUDE_FROM_ALL)
  set_directory_properties(PROPERTIES COMPILE_OPTIONS "${_opts}")
endif()

add_subdirectory(core)
enable_testing()
add_subdirectory(core/test)
if(DMODEM_GUI_SSH)
  add_subdirectory(ssh)
endif()

if(WIN32)
  add_subdirectory(windows)
elseif(DMODEM_GUI_WINPORT)
  add_subdirectory(winport)
endif()
