#ifndef ETH_PROBE_H
#define ETH_PROBE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  bool available;     /* network stack initialized */
  bool link;
  bool dhcp;          /* true: address from DHCP lease; false: static fallback or none */
  bool addressValid;
  bool powered;       /* +3.3V_ETH rail on and interface running (false while suspended) */
  char ip[16], mask[16], gw[16];
  char mac[18];
  uint16_t phyId1, phyId2;
} EthInfo;

/* Probe PHY, start CycloneTCP with a MAC derived from the STM32 UID, DHCP with a
 * static fallback (192.168.0.223/24) when no lease arrives within 10 s, and the
 * status web server. Task context, once. */
bool EthProbe_Start(void);
/* Periodic (default task): applies the DHCP fallback and logs address changes. */
void EthProbe_Report(void);
/* Power service only (Phase 4, decision S6-4). Suspend stops DHCP and the interface,
 * releases the RMII/MDIO pins to analog (no back-feed) and turns +3.3V_ETH (PE0) off.
 * Resume powers the PHY, waits until it answers on MDIO (bounded), re-initializes MAC
 * and PHY and restarts the interface (DHCP again unless the static fallback was in use).
 * Both return false when the stack was never started (e.g. safe mode) or the PHY does
 * not answer; Resume may then be retried. Task context. */
bool EthProbe_Suspend(void);
bool EthProbe_Resume(void);

/* Copy of the current network state; safe from any task. */
void EthProbe_GetInfo(EthInfo *info);

#endif
