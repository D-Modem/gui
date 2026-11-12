# The Windows app with MinGW or MSVC (the Windows 95 build is Open
# Watcom's: build-watcom.sh).  ANSI, for the same code to build with both.
enable_language(CXX)
enable_language(RC)
add_executable(dmodem WIN32
  src/main.cpp
  src/link.cpp
  src/pages.cpp
  src/term.cpp
  src/settings.cpp
  src/config.cpp
  src/voice.cpp
  src/audio.cpp
  src/termview.cpp
  src/remote.cpp
  res/app.rc)
target_include_directories(dmodem PRIVATE src)
target_link_libraries(dmodem PRIVATE dmodemctl comctl32 wsock32 comdlg32 gdi32 winmm msacm32)
if(MSVC)
  target_compile_definitions(dmodem PRIVATE _CRT_SECURE_NO_WARNINGS)
  # (a manifest of our own in app.rc)
  target_link_options(dmodem PRIVATE /MANIFEST:NO)
else()
  target_compile_options(dmodem PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_link_options(dmodem PRIVATE -static)
endif()
