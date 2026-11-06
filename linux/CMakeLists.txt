# The Linux (and BSD) app: GTK 4, libadwaita and VTE over libdmodemctl.
add_executable(dmodem-gui
  src/main.c
  src/window.c
  src/modem.c
  src/config.c
  src/settings.c
  src/stats.c)
target_link_libraries(dmodem-gui PRIVATE dmodemctl PkgConfig::GTK)
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
