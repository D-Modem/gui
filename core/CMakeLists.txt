# libdmodemctl: what the desktop apps share, in plain C with no
# dependencies.  Static for the Linux app; a DLL for the Windows app's
# P/Invoke (-DDMODEMCTL_SHARED=ON).
option(DMODEMCTL_SHARED "Build libdmodemctl as a shared library" OFF)

set(DMODEMCTL_SOURCES
  src/session.c
  src/model.c
  src/diag.c
  src/path.c
  src/vt.c
  src/vt_charsets.c
  src/vt_data.c
  src/vt_dec.c
  src/vt_fonts.c
  src/vt_gfx.c
  src/vt_keys.c
  src/vt_machine.c
  src/vt_profiles.c
  src/vt_regis.c
  src/vt_rip.c
  src/vt_vt52.c
  src/daemon.c
  src/voice.c
  src/net.c
  src/util.c
  src/xfer.c
  src/xfer_hex.c
  src/xfer_kermit.c
  src/xfer_xy.c
  src/xfer_z.c
  src/zstd_codec.c)

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
  find_package(Threads REQUIRED)
  target_link_libraries(dmodemctl PRIVATE m Threads::Threads)
endif()
