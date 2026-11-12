# mbedTLS's config with threading on, as libssh wants it: its own
# pthread mutexes, or (Windows) libssh's given through threading_alt.h.
# Run in mbedTLS's source tree by BundledLibssh.cmake.
set(_config include/mbedtls/mbedtls_config.h)
file(READ ${_config} _text)
if(NOT _text MATCHES "D-Modem: threading")
  file(APPEND ${_config} "
/* D-Modem: threading, for libssh */
#define MBEDTLS_THREADING_C
#if defined(_WIN32)
#define MBEDTLS_THREADING_ALT
#else
#define MBEDTLS_THREADING_PTHREAD
#endif
")
endif()
file(WRITE include/mbedtls/threading_alt.h "/* D-Modem: libssh's mutexes (a pointer each) */
#ifndef DMODEM_THREADING_ALT_H
#define DMODEM_THREADING_ALT_H
typedef struct mbedtls_threading_mutex_t {
    void *mutex;
} mbedtls_threading_mutex_t;
#endif
")
