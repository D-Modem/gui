# xcconfig.cmake - makes macos-embed.xcconfig from the link command that
# link-capture.sh wrote (cmake/MacosEmbed.cmake). It keeps the static
# libraries, the -l libraries and the frameworks of the command. A
# relative path is made absolute from the directory of the command.
#
# Usage: cmake -DIN=<captured command> -DOUT=<xcconfig>
#          -DHEADERS=<directory of dmodem_embed.h> -P xcconfig.cmake

file(STRINGS ${IN} _args)
list(POP_FRONT _args _dir)
set(_ld "")
set(_framework OFF)
foreach(_a ${_args})
  if(_framework)
    string(APPEND _ld " -framework ${_a}")
    set(_framework OFF)
  elseif(_a STREQUAL "-framework")
    set(_framework ON)
  elseif(_a MATCHES "^-framework ")
    string(APPEND _ld " ${_a}")
  elseif(_a MATCHES "^-l" OR _a STREQUAL "-pthread")
    string(APPEND _ld " ${_a}")
  elseif(_a MATCHES "\\.a$")
    get_filename_component(_p "${_a}" ABSOLUTE BASE_DIR "${_dir}")
    string(APPEND _ld " \"${_p}\"")
  endif()
endforeach()
# (the modem is C++: the app, Swift and C, links libc++ for it)
if(NOT _ld MATCHES " -lc\\+\\+")
  string(APPEND _ld " -lc++")
endif()
file(WRITE ${OUT}.new
"// Made by CMake (cmake/MacosEmbed.cmake): the built-in modem
DMODEM_EMBED_HEADERS = \"${HEADERS}\"
DMODEM_EMBED_LDFLAGS =${_ld}
DMODEM_EMBED_SWIFT = DMODEM_GUI_EMBED
")
# (the same file is not written again, so Xcode does not rebuild)
file(COPY_FILE ${OUT}.new ${OUT} ONLY_IF_DIFFERENT)
file(REMOVE ${OUT}.new)
