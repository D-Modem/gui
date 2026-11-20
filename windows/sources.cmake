# The sources of the Windows app, for windows/ and winport/ (the same
# app on Linux and macOS)
set(DMODEM_WIN_SOURCES
  main.cpp
  link.cpp
  pages.cpp
  term.cpp
  settings.cpp
  pathview.cpp
  config.cpp
  voice.cpp
  audio.cpp
  termview.cpp
  remote.cpp
  xfer.cpp
  capture.cpp)
list(TRANSFORM DMODEM_WIN_SOURCES PREPEND "${CMAKE_CURRENT_LIST_DIR}/src/")
