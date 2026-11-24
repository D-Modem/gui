# The macOS app (macos/) builds with Xcode, not CMake. This file writes
# ${CMAKE_BINARY_DIR}/macos-embed.xcconfig, which the Xcode project
# (macos/project.yml) includes:
#   DMODEM_EMBED_HEADERS  the directory of dmodem_embed.h
#   DMODEM_EMBED_LDFLAGS  the static libraries of libdmodem_embed, all of
#                         them, and the system libraries they need
#   DMODEM_EMBED_SWIFT    DMODEM_GUI_EMBED, for Swift's #if
# Without the built-in modem, the three values are empty.
#
# A small program, dmodem_embed_link, links the library. Its linker
# launcher (macos-embed/link-capture.sh) keeps the link command that
# CMake made, with all the libraries that the library needs. Then
# macos-embed/xcconfig.cmake makes the file from that command.

set(_xcconfig ${CMAKE_BINARY_DIR}/macos-embed.xcconfig)
if(DMODEM_GUI_EMBED AND TARGET dmodem_embed)
  if(CMAKE_VERSION VERSION_LESS 3.21)
    message(FATAL_ERROR "The built-in modem of the macOS app needs CMake 3.21 or later")
  endif()
  set(_dir ${CMAKE_CURRENT_LIST_DIR}/macos-embed)
  set(_cmd ${CMAKE_BINARY_DIR}/macos-embed.link)
  add_executable(dmodem_embed_link ${_dir}/main.c)
  target_link_libraries(dmodem_embed_link PRIVATE dmodem_embed)
  set_target_properties(dmodem_embed_link PROPERTIES
    LINKER_LANGUAGE CXX
    C_LINKER_LAUNCHER "${_dir}/link-capture.sh;${_cmd}"
    CXX_LINKER_LAUNCHER "${_dir}/link-capture.sh;${_cmd}")
  add_custom_command(TARGET dmodem_embed_link POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DIN=${_cmd} -DOUT=${_xcconfig}
      -DHEADERS=${DMODEM_GUI_DMODEM_DIR}/frontend/embed
      -P ${_dir}/xcconfig.cmake
    VERBATIM)
else()
  file(WRITE ${_xcconfig}
"// Made by CMake (cmake/MacosEmbed.cmake): no built-in modem
DMODEM_EMBED_HEADERS =
DMODEM_EMBED_LDFLAGS =
DMODEM_EMBED_SWIFT =
")
endif()
