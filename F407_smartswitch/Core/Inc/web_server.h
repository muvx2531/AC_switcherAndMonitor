#ifndef WEB_SERVER_H
#define WEB_SERVER_H
#include <stdbool.h>
#include "core/net.h"

/* Read-only status web (Phase 0.5, docs/web-status-api.md): serves the page from
 * res.c and GET /api/status. No relay control and no login in this phase.
 * Call once from task context after the network interface is configured. */
bool WebServer_Start(NetInterface *interface);

#endif
