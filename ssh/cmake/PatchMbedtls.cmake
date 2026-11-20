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

# Windows before Vista (the Windows 95 build of clang-retro): BCryptGenRandom
# is not there.  The entropy comes from CryptGenRandom of CryptoAPI, found
# at run time, so that the app also starts on a Windows 95 without
# CryptoAPI (then SSH alone fails: no entropy source).
set(_poll library/entropy_poll.c)
file(READ ${_poll} _text)
if(NOT _text MATCHES "D-Modem: CryptGenRandom")
  set(_old "#include <windows.h>\n#include <bcrypt.h>\n#include <intsafe.h>\n")
  set(_new "#include <windows.h>\n
/* D-Modem: CryptGenRandom before Vista, found at run time */
#if defined(_WIN32_WINNT) && _WIN32_WINNT < 0x0600
#include <wincrypt.h>

typedef BOOL (WINAPI *dmodem_acquire_fn)(HCRYPTPROV *, LPCSTR, LPCSTR,
                                         DWORD, DWORD);
typedef BOOL (WINAPI *dmodem_gen_random_fn)(HCRYPTPROV, DWORD, BYTE *);
typedef BOOL (WINAPI *dmodem_release_fn)(HCRYPTPROV, DWORD);

int mbedtls_platform_entropy_poll(void *data, unsigned char *output, size_t len,
                                  size_t *olen)
{
    HMODULE lib;
    dmodem_acquire_fn acquire;
    dmodem_gen_random_fn gen_random;
    dmodem_release_fn release;
    HCRYPTPROV prov;
    BOOL ok;

    ((void) data);
    *olen = 0;
    lib = LoadLibraryA(\"advapi32.dll\");
    if (lib == NULL) {
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    }
    acquire = (dmodem_acquire_fn) (void (*)(void))
        GetProcAddress(lib, \"CryptAcquireContextA\");
    gen_random = (dmodem_gen_random_fn) (void (*)(void))
        GetProcAddress(lib, \"CryptGenRandom\");
    release = (dmodem_release_fn) (void (*)(void))
        GetProcAddress(lib, \"CryptReleaseContext\");
    if (acquire == NULL || gen_random == NULL || release == NULL ||
        len > 0xffffffffu ||
        !acquire(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    }
    ok = gen_random(prov, (DWORD) len, output);
    release(prov, 0);
    if (!ok) {
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    }
    *olen = len;
    return 0;
}
#else /* D-Modem: BCryptGenRandom */
#include <bcrypt.h>
#include <intsafe.h>
")
  string(FIND "${_text}" "${_old}" _at)
  if(_at LESS 0)
    message(FATAL_ERROR "PatchMbedtls: entropy_poll.c has changed")
  endif()
  string(REPLACE "${_old}" "${_new}" _text "${_text}")
  string(REPLACE "#else /* _WIN32 && !EFIX64 && !EFI32 */"
    "#endif /* D-Modem: BCryptGenRandom */\n#else /* _WIN32 && !EFIX64 && !EFI32 */"
    _text "${_text}")
  file(WRITE ${_poll} "${_text}")
endif()

# picolibc (the Windows 95 build): it has gmtime_r and not the gmtime_s of
# Microsoft, and memset_s only when <string.h> sees __STDC_WANT_LIB_EXT1__
set(_util library/platform_util.c)
file(READ ${_util} _text)
if(NOT _text MATCHES "__PICOLIBC__")
  string(REPLACE "#if defined(_WIN32) && !defined(PLATFORM_UTIL_USE_GMTIME)\n"
    "#if defined(_WIN32) && !defined(PLATFORM_UTIL_USE_GMTIME) && !defined(__PICOLIBC__)\n"
    _text "${_text}")
  string(REPLACE "#elif defined(__STDC_LIB_EXT1__) && !defined(__IAR_SYSTEMS_ICC__)\n"
    "#elif defined(__STDC_LIB_EXT1__) && !defined(__IAR_SYSTEMS_ICC__) && !defined(__PICOLIBC__)\n"
    _text "${_text}")
  file(WRITE ${_util} "${_text}")
endif()
