# The Windows app (windows/) for Linux and macOS, over the portable Win32
# layer of clang-retro MFC (libwinport, in port/ of that project).
#
#   cmake -B build -DDMODEM_GUI_WINPORT=ON -DCLANG_RETRO_MFC=/path/to/mfc \
#         -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
enable_language(CXX)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "DMODEM_GUI_WINPORT: use clang++ (libwinport is built with clang)")
endif()
if(NOT CLANG_RETRO_MFC OR NOT EXISTS "${CLANG_RETRO_MFC}/port/include/windows.h")
  message(FATAL_ERROR "DMODEM_GUI_WINPORT: set CLANG_RETRO_MFC to a clang-retro MFC checkout")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
# (Homebrew's LLVM has llvm-rc; Xcode does not)
find_program(DMODEM_LLVM_RC NAMES llvm-rc llvm-rc-20 llvm-rc-19 llvm-rc-18
  HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin REQUIRED)

# The target triple of the layer's Makefile. On macOS, the
# architecture is CMAKE_OSX_ARCHITECTURES when it is set, else the
# architecture of this Mac. Thus a Mac with Apple silicon can build the
# app for an Intel Mac: -DCMAKE_OSX_ARCHITECTURES=x86_64. The layer
# builds for one architecture only.
set(_winport_arch "${CMAKE_SYSTEM_PROCESSOR}")
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
  list(LENGTH CMAKE_OSX_ARCHITECTURES _n_archs)
  if(NOT _n_archs EQUAL 1)
    message(FATAL_ERROR "DMODEM_GUI_WINPORT: set one architecture in "
      "CMAKE_OSX_ARCHITECTURES (arm64 or x86_64), not \"${CMAKE_OSX_ARCHITECTURES}\"")
  endif()
  set(_winport_arch "${CMAKE_OSX_ARCHITECTURES}")
endif()
if(_winport_arch MATCHES "^(arm64|aarch64)$")
  set(_arch_linux aarch64)
  set(_arch_apple arm64)
else()
  set(_arch_linux x86_64)
  set(_arch_apple x86_64)
endif()
if(APPLE)
  set(WINPORT_TARGET "${_arch_apple}-apple-macos11")
else()
  set(WINPORT_TARGET "${_arch_linux}-linux-gnu")
endif()

# libwinport.a, built by the MFC Makefile into our build directory
set(WINPORT_OUT "${CMAKE_CURRENT_BINARY_DIR}/mfc")
set(WINPORT_LIB "${WINPORT_OUT}/libwinport.a")
file(GLOB _winport_deps CONFIGURE_DEPENDS
  "${CLANG_RETRO_MFC}/port/src/*" "${CLANG_RETRO_MFC}/port/include/*.h")
include(ProcessorCount)
ProcessorCount(_ncpu)
if(_ncpu EQUAL 0)
  set(_ncpu 1)
endif()
add_custom_command(OUTPUT "${WINPORT_LIB}"
  COMMAND make -C "${CLANG_RETRO_MFC}" -j${_ncpu}
          TARGET=${WINPORT_TARGET} STDLIB= OUT=${WINPORT_OUT}
          CXX=${CMAKE_CXX_COMPILER} AR=${CMAKE_AR} "${WINPORT_LIB}"
  DEPENDS ${_winport_deps}
  COMMENT "Building libwinport.a (${WINPORT_TARGET})"
  VERBATIM)
add_custom_target(winport_lib DEPENDS "${WINPORT_LIB}")
add_library(winport STATIC IMPORTED GLOBAL)
set_target_properties(winport PROPERTIES IMPORTED_LOCATION "${WINPORT_LIB}")
add_dependencies(winport winport_lib)

# The resources: app.rc as C++ (res2cpp.py compiles it with llvm-rc)
set(_win "${PROJECT_SOURCE_DIR}/windows")
set(APP_RES_CPP "${CMAKE_CURRENT_BINARY_DIR}/app-res.cpp")
add_custom_command(OUTPUT "${APP_RES_CPP}"
  COMMAND ${CMAKE_COMMAND} -E env RC=${DMODEM_LLVM_RC} CXX=${CMAKE_CXX_COMPILER}
          ${Python3_EXECUTABLE} "${CLANG_RETRO_MFC}/port/tools/res2cpp.py"
          -I "${_win}/src" -I "${_win}/res" "${_win}/res/app.rc" -o "${APP_RES_CPP}"
  DEPENDS "${_win}/res/app.rc" "${_win}/res/app.manifest" "${_win}/res/dmodem.ico"
          "${_win}/src/resource.h"
  COMMENT "Converting app.rc to C++"
  VERBATIM)

include("${_win}/sources.cmake")
add_executable(dmodem-win32
  ${DMODEM_WIN_SOURCES}
  "${APP_RES_CPP}")
set_target_properties(dmodem-win32 PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(dmodem-win32 BEFORE PRIVATE
  "${CLANG_RETRO_MFC}/port/include" "${_win}/src" "${_win}/res")
target_compile_definitions(dmodem-win32 PRIVATE DMODEM_WINPORT)
target_compile_options(dmodem-win32 PRIVATE -Wall -Wno-unused-parameter)
target_link_libraries(dmodem-win32 PRIVATE dmodemctl winport)
if(TARGET dmodemssh)
  target_link_libraries(dmodem-win32 PRIVATE dmodemssh)
  target_compile_definitions(dmodem-win32 PRIVATE DMODEM_HAVE_SSH)
endif()
# the modem built in (libdmodem_embed, DMODEM_GUI_EMBED): the dmodemd
# line starts no dmodemd (windows/src/link.cpp)
if(DMODEM_GUI_EMBED AND TARGET dmodem_embed)
  target_link_libraries(dmodem-win32 PRIVATE dmodem_embed)
  target_compile_definitions(dmodem-win32 PRIVATE DMODEM_GUI_EMBED)
endif()
if(APPLE)
  target_link_libraries(dmodem-win32 PRIVATE "-framework Cocoa" pthread)
else()
  target_link_libraries(dmodem-win32 PRIVATE dl pthread)
endif()
