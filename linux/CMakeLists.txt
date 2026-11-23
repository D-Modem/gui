# The Linux (and BSD) app: GTK 4 and libadwaita over libdmodemctl
# (and libdmodemssh for the Remote page's SSH).
add_executable(dmodem-gui
  src/main.c
  src/window.c
  src/modem.c
  src/config.c
  src/usbmodem.c
  src/settings.c
  src/pathview.c
  src/stats.c
  src/voice.c
  src/remote.c
  src/termview.c
  src/audio.c)
target_link_libraries(dmodem-gui PRIVATE dmodemctl dmodemssh PkgConfig::GTK)
# the modem built in (libdmodem_embed): no dmodemd to start
if(DMODEM_GUI_EMBED AND TARGET dmodem_embed)
  target_link_libraries(dmodem-gui PRIVATE dmodem_embed)
  target_compile_definitions(dmodem-gui PRIVATE DMODEM_GUI_EMBED)
  set_target_properties(dmodem-gui PROPERTIES LINKER_LANGUAGE CXX)
endif()
# where the MT9234MU's firmware is installed, and where it is in the source
include(GNUInstallDirs)
target_compile_definitions(dmodem-gui PRIVATE
  DMODEM_FIRMWARE_DIR="${CMAKE_INSTALL_FULL_DATADIR}/dmodem-gui/firmware"
  DMODEM_FIRMWARE_SRC="${CMAKE_CURRENT_SOURCE_DIR}/firmware")
# the sound of voice calls: PulseAudio's client (PipeWire serves it
# too); without it the app has no sound
pkg_check_modules(PULSE IMPORTED_TARGET libpulse-mainloop-glib)
if(PULSE_FOUND)
  target_link_libraries(dmodem-gui PRIVATE PkgConfig::PULSE)
  target_compile_definitions(dmodem-gui PRIVATE HAVE_PULSE)
else()
  message(STATUS "libpulse-mainloop-glib not found: the Linux app has no sound for voice calls")
endif()
# greetings from MP3, FLAC, Ogg and the like (WAV, AIFF and .au need
# nothing more)
pkg_check_modules(SNDFILE IMPORTED_TARGET sndfile)
if(SNDFILE_FOUND)
  target_link_libraries(dmodem-gui PRIVATE PkgConfig::SNDFILE)
  target_compile_definitions(dmodem-gui PRIVATE HAVE_SNDFILE)
else()
  message(STATUS "libsndfile not found: greetings are read from WAV, AIFF and .au files only")
endif()
target_compile_options(dmodem-gui PRIVATE -Wall -Wextra -Wno-unused-parameter)

include(GNUInstallDirs)
install(TARGETS dmodem-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/io.github.d_modem.DModem.desktop
  DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/io.github.d_modem.DModem.metainfo.xml
  DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
# MultiTech's firmware for the MT9234MU (licence unknown: linux/firmware/README.md)
install(FILES firmware/mts_mt9234mu.fw firmware/README.md
  DESTINATION ${CMAKE_INSTALL_DATADIR}/dmodem-gui/firmware)
option(DMODEM_GUI_INSTALL_FIRMWARE
  "Also install mts_mt9234mu.fw (and mts_mt9234zba.fw) in /lib/firmware" OFF)
if(DMODEM_GUI_INSTALL_FIRMWARE)
  install(FILES firmware/mts_mt9234mu.fw DESTINATION /lib/firmware)
  install(FILES firmware/mts_mt9234mu.fw DESTINATION /lib/firmware
    RENAME mts_mt9234zba.fw)
endif()
install(FILES ${PROJECT_SOURCE_DIR}/assets/dmodem.svg
  DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
  RENAME io.github.d_modem.DModem.svg)
