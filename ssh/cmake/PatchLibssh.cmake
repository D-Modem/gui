# libssh's remote listen on port 0 told the port the server picked from
# its own reply: libssh reads it from whatever packet came in last, which
# is often another one (its own "FIXME"), and then fails a listen that
# the server took. The reply's port kept in the session as it comes.
# Run in libssh's source tree by BundledLibssh.cmake.

# (a replace that must take: else this libssh isn't the one patched for)
function(dmodem_patch file from to)
  file(READ ${file} _text)
  string(FIND "${_text}" "${to}" _done)
  if(_done GREATER -1)
    return()
  endif()
  string(FIND "${_text}" "${from}" _at)
  if(_at LESS 0)
    message(FATAL_ERROR "PatchLibssh: ${file} isn't as expected")
  endif()
  string(REPLACE "${from}" "${to}" _text "${_text}")
  file(WRITE ${file} "${_text}")
endfunction()

dmodem_patch(include/libssh/session.h
  "    enum ssh_channel_request_state_e global_req_state;"
  "    enum ssh_channel_request_state_e global_req_state;
    /* D-Modem: bound port, from a global request's success */
    int dmodem_bound_port;")

dmodem_patch(src/channels.c
  "  } else {
    session->global_req_state=SSH_CHANNEL_REQ_STATE_ACCEPTED;
  }"
  "  } else {
    /* D-Modem: bound port */
    uint32_t bound = 0;
    session->global_req_state=SSH_CHANNEL_REQ_STATE_ACCEPTED;
    session->dmodem_bound_port =
        ssh_buffer_unpack(packet, \"d\", &bound) == SSH_OK ? (int)bound : 0;
  }")

dmodem_patch(src/channels.c
  "  if (rc == SSH_OK && port == 0 && bound_port != NULL) {
    rc = ssh_buffer_unpack(session->in_buffer, \"d\", bound_port);
    if (rc != SSH_OK)
        *bound_port = 0;
  }"
  "  if (rc == SSH_OK && port == 0 && bound_port != NULL) {
    /* D-Modem: bound port, as the success packet had it */
    *bound_port = session->dmodem_bound_port;
  }")

# Windows before Vista (the Windows 95 build of clang-retro): misc.c's
# SHGetSpecialFolderPathA (95 and NT 4.0 have it only with the shell of
# Internet Explorer 4), if_nametoindex (Vista) and WSAStringToAddressA
# (Winsock 2, which the first Windows 95 does not have) found at run
# time, so that the app also starts where they are not there.
dmodem_patch(src/misc.c
  "#include <netioapi.h>
"
  "#include <netioapi.h>

/* D-Modem: functions found at run time before Vista */
#if defined(_WIN32_WINNT) && _WIN32_WINNT < 0x0600
static FARPROC dmodem_proc(const char *dll, const char *name)
{
    HMODULE lib = GetModuleHandleA(dll);

    if (lib == NULL) {
        lib = LoadLibraryA(dll);
    }
    return lib != NULL ? GetProcAddress(lib, name) : NULL;
}

/* (no SHGetSpecialFolderPathA: USERPROFILE, which NT 4.0 has) */
static BOOL dmodem_profile_dir(char *dir)
{
    typedef BOOL (WINAPI *fn_t)(HWND, LPSTR, int, BOOL);
    fn_t fn = (fn_t) (void (*)(void))
        dmodem_proc(\"shell32.dll\", \"SHGetSpecialFolderPathA\");
    const char *env;

    if (fn != NULL && fn(NULL, dir, CSIDL_PROFILE, TRUE)) {
        return TRUE;
    }
    env = getenv(\"USERPROFILE\");
    if (env != NULL && *env != '\\0' && strlen(env) < PATH_MAX) {
        strcpy(dir, env);
        return TRUE;
    }
    return FALSE;
}

/* (no if_nametoindex: no interface names) */
static ULONG dmodem_if_nametoindex(const char *name)
{
    typedef ULONG (WINAPI *fn_t)(PCSTR);
    fn_t fn = (fn_t) (void (*)(void))
        dmodem_proc(\"iphlpapi.dll\", \"if_nametoindex\");

    return fn != NULL ? fn(name) : 0;
}

/* (no Winsock 2: IPv4 alone, by inet_addr of Winsock 1.1) */
static INT dmodem_WSAStringToAddressA(LPSTR str, INT family,
                                      LPWSAPROTOCOL_INFOA info,
                                      LPSOCKADDR addr, LPINT len)
{
    typedef INT (WSAAPI *fn_t)(LPSTR, INT, LPWSAPROTOCOL_INFOA, LPSOCKADDR,
                               LPINT);
    fn_t fn = (fn_t) (void (*)(void))
        dmodem_proc(\"ws2_32.dll\", \"WSAStringToAddressA\");
    struct sockaddr_in *sin = (struct sockaddr_in *) addr;
    unsigned long a;

    if (fn != NULL) {
        return fn(str, family, info, addr, len);
    }
    if (family != AF_INET || *len < (INT) sizeof(*sin)) {
        return SOCKET_ERROR;
    }
    a = inet_addr(str);
    if (a == INADDR_NONE && strcmp(str, \"255.255.255.255\") != 0) {
        return SOCKET_ERROR;
    }
    memset(sin, 0, sizeof(*sin));
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = a;
    *len = sizeof(*sin);
    return 0;
}
#else
#define dmodem_profile_dir(dir) SHGetSpecialFolderPathA(NULL, (dir), CSIDL_PROFILE, TRUE)
#define dmodem_if_nametoindex if_nametoindex
#define dmodem_WSAStringToAddressA WSAStringToAddressA
#endif
")
dmodem_patch(src/misc.c
  "  if (SHGetSpecialFolderPathA(NULL, tmp, CSIDL_PROFILE, TRUE)) {"
  "  if (dmodem_profile_dir(tmp)) {")
dmodem_patch(src/misc.c
  "rc = if_nametoindex(network_interface + 1);"
  "rc = dmodem_if_nametoindex(network_interface + 1);")
dmodem_patch(src/misc.c
  "rc = WSAStringToAddressA((LPSTR) "
  "rc = dmodem_WSAStringToAddressA((LPSTR) ")
# (the other ssh_is_ipaddr, for systems other than Windows, keeps its own)
dmodem_patch(src/misc.c
  "#else /* _WIN32 */
"
  "#else /* _WIN32 */

#define dmodem_if_nametoindex if_nametoindex /* D-Modem */
")
