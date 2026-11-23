# libssh on mbedTLS, both static, built from their release tarballs
# (checked by hash), as the target dmodem_libssh.
include(FetchContent)
include(ExternalProject)

set(DMODEM_MBEDTLS_URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2)
set(DMODEM_MBEDTLS_SHA256 a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6)
set(DMODEM_LIBSSH_URL https://www.libssh.org/files/0.11/libssh-0.11.3.tar.xz)
set(DMODEM_LIBSSH_SHA256 7d8a1361bb094ec3f511964e78a5a4dba689b5986e112afabe4f4d0d6c6125c3)

set(_prefix ${CMAKE_BINARY_DIR}/dmodem-libssh)
set(_args
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_INSTALL_PREFIX=${_prefix}
  -DCMAKE_INSTALL_LIBDIR=lib
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON
  -DCMAKE_MSVC_RUNTIME_LIBRARY=${CMAKE_MSVC_RUNTIME_LIBRARY}
  -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
  -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
  -DCMAKE_OSX_SYSROOT=${CMAKE_OSX_SYSROOT})
# (a toolchain file names the compiler itself)
if(CMAKE_TOOLCHAIN_FILE)
  list(APPEND _args -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE})
else()
  list(APPEND _args -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
endif()
# (cmake/clang-retro.cmake)
if(CLANG_DOS)
  list(APPEND _args -DCLANG_DOS=${CLANG_DOS} -DCLANG_RETRO_CONFIG=${CLANG_RETRO_CONFIG})
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  list(APPEND _args -DCMAKE_SYSTEM_NAME=iOS)
endif()
if(ANDROID)
  list(APPEND _args -DANDROID_ABI=${ANDROID_ABI} -DANDROID_PLATFORM=${ANDROID_PLATFORM})
endif()
if(CMAKE_MAKE_PROGRAM)
  list(APPEND _args -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM})
endif()
string(REPLACE ";" "|" _archs "${CMAKE_OSX_ARCHITECTURES}")
list(TRANSFORM _args REPLACE "^-DCMAKE_OSX_ARCHITECTURES=.*" "-DCMAKE_OSX_ARCHITECTURES=${_archs}")

if(WIN32)
  set(_mbed_libs mbedcrypto.lib mbedx509.lib mbedtls.lib everest.lib p256m.lib)
  set(_ssh_lib ssh.lib)
  if(MINGW)
    set(_mbed_libs libmbedcrypto.a libmbedx509.a libmbedtls.a libeverest.a libp256m.a)
    set(_ssh_lib libssh.a)
  endif()
else()
  set(_mbed_libs libmbedcrypto.a libmbedx509.a libmbedtls.a libeverest.a libp256m.a)
  set(_ssh_lib libssh.a)
endif()
list(TRANSFORM _mbed_libs PREPEND ${_prefix}/lib/ OUTPUT_VARIABLE _mbed_paths)

ExternalProject_Add(dmodem_mbedtls
  URL ${DMODEM_MBEDTLS_URL}
  URL_HASH SHA256=${DMODEM_MBEDTLS_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  PATCH_COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/PatchMbedtls.cmake
  LIST_SEPARATOR |
  CMAKE_ARGS ${_args} -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF
             -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DMBEDTLS_FATAL_WARNINGS=OFF
  BUILD_BYPRODUCTS ${_mbed_paths})

# libssh's sha1() and sha256() renamed: libre (in the modem built in,
# DMODEM_GUI_EMBED) has functions of the same names
set(_ssh_args)
set(_ssh_cflags "-Dsha1=dmodem_ssh_sha1 -Dsha256=dmodem_ssh_sha256")
# (Bionic has no S_IWRITE, the old name libssh uses)
if(ANDROID)
  string(APPEND _ssh_cflags " -DS_IWRITE=S_IWUSR")
endif()
# clang-retro (cmake/clang-retro.cmake): its clang, newer than MinGW's
# GCC, makes an error of src/threads/mbedtls.c's casts of libssh's mutex
# callbacks.  With picolibc (the Windows 95 build): strdup and
# explicit_bzero need _DEFAULT_SOURCE, and strerror_s needs
# __STDC_WANT_LIB_EXT1__; it has no <direct.h> (picolibc/direct.h here);
# its <io.h> and <termios.h> are not those that libssh expects on
# Windows, and its <arpa/inet.h> conflicts with <winsock2.h>, so libssh
# uses none of them.
if(CLANG_DOS)
  string(APPEND _ssh_cflags " -Wno-error=incompatible-function-pointer-types")
  if(CLANG_RETRO_CONFIG MATCHES "picolibc")
    string(APPEND _ssh_cflags " -D_DEFAULT_SOURCE -D__STDC_WANT_LIB_EXT1__=1 -I${CMAKE_CURRENT_LIST_DIR}/picolibc")
    list(APPEND _ssh_args -DHAVE_ARPA_INET_H= -DHAVE_IO_H= -DHAVE_TERMIOS_H=)
  endif()
endif()
list(APPEND _ssh_args "-DCMAKE_C_FLAGS=${_ssh_cflags}")

# (WITH_SERVER: libssh runs a client's callbacks for the channels the
# server opens, the forwarded agent's, only when it's built with it)
ExternalProject_Add(dmodem_libssh_build
  URL ${DMODEM_LIBSSH_URL}
  URL_HASH SHA256=${DMODEM_LIBSSH_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  DEPENDS dmodem_mbedtls
  PATCH_COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/PatchLibssh.cmake
  LIST_SEPARATOR |
  CMAKE_ARGS ${_args} ${_ssh_args} -DBUILD_SHARED_LIBS=OFF
             -DWITH_MBEDTLS=ON -DMBEDTLS_ROOT_DIR=${_prefix}
             -DCMAKE_PREFIX_PATH=${_prefix} -DCMAKE_FIND_ROOT_PATH=${_prefix}
             -DWITH_ZLIB=OFF -DWITH_GSSAPI=OFF -DWITH_SERVER=ON
             -DWITH_EXAMPLES=OFF -DUNIT_TESTING=OFF -DCLIENT_TESTING=OFF
             -DWITH_NACL=OFF -DWITH_SYMBOL_VERSIONING=OFF
  BUILD_BYPRODUCTS ${_prefix}/lib/${_ssh_lib})

file(MAKE_DIRECTORY ${_prefix}/include)
add_library(dmodem_libssh INTERFACE)
add_dependencies(dmodem_libssh dmodem_libssh_build)
target_include_directories(dmodem_libssh INTERFACE ${_prefix}/include)
target_compile_definitions(dmodem_libssh INTERFACE LIBSSH_STATIC=1)
target_link_libraries(dmodem_libssh INTERFACE ${_prefix}/lib/${_ssh_lib} ${_mbed_paths})
if(WIN32)
  target_link_libraries(dmodem_libssh INTERFACE ws2_32 iphlpapi bcrypt shell32 advapi32)
  # (MinGW's libssh locks with winpthreads; clang-retro's sysroots have
  # none, and there libssh locks with Win32's own)
  if(MINGW AND NOT CLANG_DOS)
    target_link_libraries(dmodem_libssh INTERFACE pthread)
  endif()
elseif(UNIX)
  find_package(Threads REQUIRED)
  target_link_libraries(dmodem_libssh INTERFACE Threads::Threads)
endif()
