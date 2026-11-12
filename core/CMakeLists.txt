# libdmodemctl: what the desktop apps share, in plain C with no
# dependencies.  Static for the Linux app; a DLL for the Windows app's
# P/Invoke (-DDMODEMCTL_SHARED=ON).
option(DMODEMCTL_SHARED "Build libdmodemctl as a shared library" OFF)

set(DMODEMCTL_SOURCES
  src/session.c
  src/model.c
  src/diag.c
  src/vt.c
  src/daemon.c
  src/voice.c
  src/net.c
  src/util.c)

if(DMODEMCTL_SHARED)
  add_library(dmodemctl SHARED ${DMODEMCTL_SOURCES})
  target_compile_definitions(dmodemctl PRIVATE DMC_BUILD_DLL)
  set_target_properties(dmodemctl PROPERTIES C_VISIBILITY_PRESET hidden)
else()
  add_library(dmodemctl STATIC ${DMODEMCTL_SOURCES})
endif()
target_include_directories(dmodemctl PUBLIC include)
if(MSVC)
  target_compile_options(dmodemctl PRIVATE /W4 /wd4996)
else()
  target_compile_options(dmodemctl PRIVATE -Wall -Wextra -Wshadow)
endif()
if(UNIX)
  target_link_libraries(dmodemctl PRIVATE m)
endif()
