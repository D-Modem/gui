# The platform file of the classic Mac OS (cmake/macos9.cmake): a POSIX
# system for CMake, over the C library of clang-retro/mac-sdk. A program
# is a MacBinary II file (.bin) that mac-sdk's tools/macapp.py links.
set(UNIX 1)
set(CMAKE_STATIC_LIBRARY_PREFIX "lib")
set(CMAKE_STATIC_LIBRARY_SUFFIX ".a")
set(CMAKE_EXECUTABLE_SUFFIX ".bin")
set(CMAKE_FIND_LIBRARY_PREFIXES "lib")
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
set_property(GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS FALSE)
