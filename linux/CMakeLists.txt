# The Linux (and BSD) app: GTK 4, libadwaita and VTE over libdmodemctl.
add_executable(dmodem-gui
  src/main.c
  src/window.c
  src/modem.c
  src/config.c
  src/settings.c
  src/stats.c
  src/voice.c
  src/audio.c)
target_link_libraries(dmodem-gui PRIVATE dmodemctl PkgConfig::GTK)
# the sound of voice calls: PulseAudio's client (PipeWire serves it
# too); without it the app has no sound
pkg_check_modules(PULSE IMPORTED_TARGET libpulse-mainloop-glib)
if(PULSE_FOUND)
  target_link_libraries(dmodem-gui PRIVATE PkgConfig::PULSE)
  target_compile_definitions(dmodem-gui PRIVATE HAVE_PULSE)
else()
  message(STATUS "libpulse-mainloop-glib not found: the Linux app has no sound for voice calls")
endif()
target_compile_options(dmodem-gui PRIVATE -Wall -Wextra -Wno-unused-parameter)

include(GNUInstallDirs)
install(TARGETS dmodem-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/io.github.d_modem.DModem.desktop
  DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/io.github.d_modem.DModem.metainfo.xml
  DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES ${PROJECT_SOURCE_DIR}/assets/dmodem.svg
  DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
  RENAME io.github.d_modem.DModem.svg)
