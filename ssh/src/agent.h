/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 * agent.h - the agent libdmodemssh logs in with (agent.c).
 *
 * This file and agent.c are the same in D-Modem/gui (ssh/) and
 * D-Modem/app (native-ssh/): change both.
 */
#ifndef DMODEM_AGENT_H
#define DMODEM_AGENT_H

#include <libssh/libssh.h>

typedef struct dmc_agent dmc_agent;

/* whether the agent answers: which is its socket (Linux, macOS; NULL or
   "": SSH_AUTH_SOCK's), or on Windows "pageant", a named pipe, or NULL
   or "" for Pageant and else OpenSSH's agent */
int dmc_agent_reachable(const char *which);

/* the session's agent set to which (as above), before its login by the
   agent: 0, or -1 when there's none; *out is freed by dmc_agent_detach()
   once the session is */
int dmc_agent_attach(ssh_session ssh, const char *which, dmc_agent **out);

/* what dmc_agent_attach() made, freed after its session (NULL: none) */
void dmc_agent_detach(dmc_agent *a);

#endif
