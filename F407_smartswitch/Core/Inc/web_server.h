#ifndef WEB_SERVER_H
#define WEB_SERVER_H
#include <stdbool.h>
#include "core/net.h"

/* Read-only status web (Phase 0.5, docs/web-status-api.md): serves the page from
 * res.c and GET /api/status. No relay control and no login in this phase.
 * Call once from task context after the network interface is configured. */
bool WebServer_Start(NetInterface *interface);

/* JSON documents shared with the remote link (same content as the HTTP API). Builds the
 * document under the builder mutex (waits up to 1 s) and passes it to sink while the mutex
 * is held, so sink must be quick and bounded. false: busy or too large (sink not called). */
typedef enum { WEB_JSON_STATUS, WEB_JSON_WAVEFORM, WEB_JSON_FW } WebJsonKind;
typedef void (*WebJsonSink)(const char *json, size_t len, void *ctx);
bool WebServer_WithJson(WebJsonKind kind, WebJsonSink sink, void *ctx);

#endif
