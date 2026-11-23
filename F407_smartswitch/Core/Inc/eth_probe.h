#ifndef ETH_PROBE_H
#define ETH_PROBE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  bool available;     /* network stack initialized */
  bool link;
  bool dhcp;          /* true: address from DHCP lease; false: static fallback or none */
  bool addressValid;
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
/* Copy of the current network state; safe from any task. */
void EthProbe_GetInfo(EthInfo *info);

#endif
