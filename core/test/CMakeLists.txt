add_executable(dmodemctl_test dmodemctl_test.c)
target_link_libraries(dmodemctl_test PRIVATE dmodemctl)
target_include_directories(dmodemctl_test PRIVATE ../src)
add_test(NAME dmodemctl COMMAND dmodemctl_test ${CMAKE_CURRENT_SOURCE_DIR})

# the file transfers: both ways in memory, and against lrzsz and C-Kermit
# when they are installed (a pseudo-terminal: POSIX only)
if(UNIX)
  add_executable(xfer_test xfer_test.c)
  target_link_libraries(xfer_test PRIVATE dmodemctl)
  target_include_directories(xfer_test PRIVATE ../src)
  add_test(NAME xfer COMMAND xfer_test)
  set_tests_properties(xfer PROPERTIES TIMEOUT 600)
endif()
