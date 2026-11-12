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
