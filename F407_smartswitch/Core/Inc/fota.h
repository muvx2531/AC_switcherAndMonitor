#ifndef FOTA_H
#define FOTA_H
#include <stdbool.h>
#include <stdint.h>
#include "ssw_image.h"

/* Application side of the firmware update (docs/phase5b-bootloader.md). Owns W25Q128
 * chip 2: staging slot writes, boot record A/B. The bootloader does the actual install,
 * keeps the backup and rolls back a trial image that does not confirm. */

typedef enum { FOTA_SLOT_EMPTY = 0, FOTA_SLOT_RECEIVING, FOTA_SLOT_VALID, FOTA_SLOT_INVALID } FotaSlotState;

typedef struct {
  /* running image */
  uint32_t run_version;
  char run_build[33];
  bool run_dev;                  /* DEV header (debugger build, no hash) */
  bool bootloader_present;
  /* boot record */
  SswBootState state;
  SswResult result;
  uint32_t result_version;
  uint32_t trial_boots;
  uint32_t backup_size;          /* 0: no rollback image */
  /* staging */
  FotaSlotState slot;
  uint32_t slot_version;
  char slot_build[33];
  uint32_t slot_size;
  uint32_t received;             /* during an upload */
  const char *slot_reason;       /* why INVALID */
  bool reboot_pending;
} FotaStatus;

/* Default task, after W25Q128_Init: read the boot record and the staging header, log. */
void Fota_Init(void);
/* Upload into the staging slot (HTTP task). Begin erases nothing yet; sectors are erased
 * as the data arrives. End verifies header + SHA-256 by reading the slot back. */
bool Fota_UploadBegin(uint32_t total, const char **reason);
bool Fota_UploadWrite(const uint8_t *data, uint32_t len);
bool Fota_UploadEnd(const char **reason);
void Fota_UploadAbort(const char *reason);
/* Mark the staging image for install / request a rollback; the reboot follows ~1.5 s later
 * from Fota_Tick (so the HTTP reply gets out). */
bool Fota_RequestInstall(const char **reason);
bool Fota_RequestRollback(const char **reason);
/* Default task, periodically: confirms a trial image after 60 s of healthy run phase and
 * performs a pending reboot. */
void Fota_Tick(void);
void Fota_GetStatus(FotaStatus *out);
const char *Fota_StateName(SswBootState s);
const char *Fota_ResultName(SswResult r);
const char *Fota_SlotName(FotaSlotState s);

#endif
