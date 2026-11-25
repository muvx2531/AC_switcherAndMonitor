#ifndef POWER_H
#define POWER_H
#include <stdbool.h>
#include <stdint.h>

/* Power service (docs/architecture/tasks/power.md, Phase 4). Owns the +3.3V_ETH (PE0,
 * through EthProbe_Suspend/Resume) and +3.3V_AMC (PC10) rails and decides the supply
 * mode. Never touches relays: Control reacts to 24 V loss by itself (rule (c)); Power
 * uses Control's confirmed coil_supply as the 24 V fact.
 *
 *   MAINS --24 V absent >= 2 s--> BACKUP --VBAT < 6.2 V (10 s) or 8 h on battery--> LOW_POWER
 *     ^-------------------- 24 V present >= 1 s ------------------------------------'
 *
 * MAINS: ETH and AMC rails on. BACKUP / LOW_POWER: both off (S6-4, ETH always off on
 * battery). LOW_POWER keeps the clock (owner decision 2026-11-25) and only marks the
 * state; Control, Acquisition (VBAT) and Supervisor keep running. */
typedef enum { POWER_MAINS = 0, POWER_BACKUP, POWER_LOW_POWER } PowerMode;

#define POWER_BACKUP_DELAY_MS   2000U
#define POWER_RETURN_DELAY_MS   1000U
#define POWER_LOW_VBAT_MV       6200
#define POWER_LOW_VBAT_HOLD_MS  10000U
#define POWER_LOW_AFTER_MS      (8UL * 3600UL * 1000UL)

typedef struct {
  PowerMode mode;
  uint32_t mode_age_ms;
  bool eth_rail;
  bool amc_rail;
  uint32_t transitions;
  uint32_t eth_resume_failures;
  const char *last_reason;
} PowerSnapshot;

/* Start after EthProbe_Start (or without it in safe mode). */
bool Power_Start(void);
void Power_GetSnapshot(PowerSnapshot *out);
const char *Power_ModeName(PowerMode m);

#endif
