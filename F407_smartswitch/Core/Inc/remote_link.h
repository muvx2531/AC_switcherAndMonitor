#ifndef REMOTE_LINK_H
#define REMOTE_LINK_H
#include <stdbool.h>
#include <stdint.h>

/* Remote link to the cabinet RPi (docs/remote-link-protocol.md): the board connects as a
 * TCP client to port 9001 of the RPi, found by scanning a range of the board's own subnet,
 * authenticates both ways with HMAC-SHA256 over a shared key, then streams the status
 * every second and events as they happen, and takes relay commands (source SRC_REMOTE).
 * Settings live in the board config (StRemoteConfig, web page Network tab). Not supervised:
 * every socket call is bounded, and a stalled link never touches the relays. */

typedef enum {
  REMOTE_DISABLED = 0,   /* enabled = 0 */
  REMOTE_NO_KEY,         /* no shared key set */
  REMOTE_NO_NETWORK,     /* ETH off, no link or no address */
  REMOTE_SCANNING,
  REMOTE_WAITING,        /* sweep found nobody, next sweep in 10 s */
  REMOTE_HANDSHAKE,
  REMOTE_CONNECTED,
} RemoteState;

typedef struct {
  RemoteState state;
  uint32_t peer_ip;          /* network order; current (CONNECTED/HANDSHAKE) or last good */
  uint32_t connected_s;      /* seconds in CONNECTED */
  uint32_t connects, auth_failures, sweeps, commands, rejected;
} RemoteStatus;

/* Default task, after EthProbe_Start (not in safe mode). */
bool RemoteLink_Start(void);
void RemoteLink_GetStatus(RemoteStatus *out);
const char *RemoteLink_StateName(RemoteState s);

#endif
