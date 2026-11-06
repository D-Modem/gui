add_executable(dmodemctl_test dmodemctl_test.c)
target_link_libraries(dmodemctl_test PRIVATE dmodemctl)
target_include_directories(dmodemctl_test PRIVATE ../src)
add_test(NAME dmodemctl COMMAND dmodemctl_test ${CMAKE_CURRENT_SOURCE_DIR})
