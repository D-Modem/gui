# D-Modem's desktop apps: the shared C core (libdmodemctl), and the
# Linux app (GTK 4 and libadwaita) when its libraries are there, or the
# Windows app with clang-retro (cmake/clang-retro.cmake), MinGW or MSVC.
# The macOS app (macos/) builds with Xcode and takes the core from here.
# With DMODEM_GUI_WINPORT, the Windows app also builds for Linux and
# macOS, over clang-retro MFC's portable Win32 layer (winport/).
# With DMODEM_GUI_EMBED, the Linux app has the modem in it
# (libdmodem_embed from the D-Modem submodule, external/D-Modem): it
# needs no dmodemd.
cmake_minimum_required(VERSION 3.16)
project(dmodem-gui VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(DMODEM_GUI_LINUX "Build the GTK 4 app (linux/)" ON)
option(DMODEM_GUI_SSH "Build libdmodemssh (libssh) for the Remote page" ON)
option(DMODEM_GUI_WINPORT "Build the Windows app for Linux or macOS (winport/)" OFF)
set(CLANG_RETRO_MFC "" CACHE PATH "A clang-retro MFC checkout, for DMODEM_GUI_WINPORT")

# the modem built in (docs/embedded-modem.md): the Linux app, when the
# submodule is there (git submodule update --init)
set(DMODEM_GUI_DMODEM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/D-Modem
  CACHE PATH "The D-Modem tree, for DMODEM_GUI_EMBED")
if(EXISTS ${DMODEM_GUI_DMODEM_DIR}/CMakeLists.txt AND UNIX AND NOT APPLE)
  set(_embed_default ON)
else()
  set(_embed_default OFF)
endif()
option(DMODEM_GUI_EMBED "Build the modem into the Linux app (libdmodem_embed)"
  ${_embed_default})
if(DMODEM_GUI_EMBED AND DMODEM_GUI_LINUX)
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
  add_subdirectory(${DMODEM_GUI_DMODEM_DIR} d-modem EXCLUDE_FROM_ALL)
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

if(DMODEM_GUI_LINUX AND UNIX AND NOT APPLE)
  find_package(PkgConfig)
  if(PkgConfig_FOUND)
    pkg_check_modules(GTK IMPORTED_TARGET gtk4>=4.10 libadwaita-1>=1.5)
  endif()
  if(GTK_FOUND)
    add_subdirectory(linux)
  else()
    message(STATUS "gtk4 or libadwaita-1 not found: the Linux app is not built")
  endif()
endif()
