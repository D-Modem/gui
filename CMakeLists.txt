# D-Modem's desktop apps: the shared C core (libdmodemctl), and the
# Linux app (GTK 4 and libadwaita) when its libraries are there.  The
# Windows app (windows/) and the macOS app (macos/) build with their own
# tools and take the core from here.
cmake_minimum_required(VERSION 3.16)
project(dmodem-gui VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(DMODEM_GUI_LINUX "Build the GTK 4 app (linux/)" ON)

add_subdirectory(core)
enable_testing()
add_subdirectory(core/test)

if(DMODEM_GUI_LINUX AND UNIX AND NOT APPLE)
  find_package(PkgConfig)
  if(PkgConfig_FOUND)
    pkg_check_modules(GTK IMPORTED_TARGET gtk4>=4.10 libadwaita-1>=1.5 vte-2.91-gtk4)
  endif()
  if(GTK_FOUND)
    add_subdirectory(linux)
  else()
    message(STATUS "gtk4, libadwaita-1 or vte-2.91-gtk4 not found: the Linux app is not built")
  endif()
endif()
