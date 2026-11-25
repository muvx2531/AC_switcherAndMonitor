#include "fota.h"
#include "ssw_sha256.h"
#include "w25q128.h"
#include "storage.h"
#include "supervisor.h"
#include "debug_log.h"
#include "acquisition.h"
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stddef.h>
#include <string.h>

#define CHIP W25Q128_CHIP_2
#define CONFIRM_AFTER_MS 60000U
#define REBOOT_DELAY_MS 1500U

static FotaStatus st;
static SswBootRecord br;
static uint32_t brNextSlot = SSW_BOOTREC_A;
static uint8_t page[256];                 /* upload page buffer (SRAM, flash driver source) */
static uint32_t pagePos, writePos, total;
static TickType_t rebootAt;
static bool confirmDone;
static uint8_t rbuf[1024] __attribute__((section(".ccmbss")));   /* verify reads */

const char *Fota_StateName(SswBootState s)
{
  switch (s) {
    case SSW_BR_PENDING: return "PENDING";
    case SSW_BR_INSTALLING: return "INSTALLING";
    case SSW_BR_TRIAL: return "TRIAL";
    case SSW_BR_ROLLBACK: return "ROLLBACK";
    default: return "IDLE";
  }
}

const char *Fota_ResultName(SswResult r)
{
  switch (r) {
    case SSW_RES_COMMITTED: return "COMMITTED";
    case SSW_RES_ROLLED_BACK: return "ROLLED_BACK";
    case SSW_RES_INSTALL_FAILED: return "INSTALL_FAILED";
    case SSW_RES_RESTORED: return "RESTORED";
    case SSW_RES_REPAIRED: return "REPAIRED";
    default: return "NONE";
  }
}

const char *Fota_SlotName(FotaSlotState s)
{
  switch (s) {
    case FOTA_SLOT_RECEIVING: return "RECEIVING";
    case FOTA_SLOT_VALID: return "VALID";
    case FOTA_SLOT_INVALID: return "INVALID";
    default: return "EMPTY";
  }
}

/* ---- boot record (same layout and rules as the bootloader) ---------------- */
static bool brValid(const SswBootRecord *r)
{
  return r->magic == SSW_BOOTREC_MAGIC && r->seq != 0xFFFFFFFFU && SswCrc32(r, offsetof(SswBootRecord, crc)) == r->crc;
}

static void brRead(void)
{
  SswBootRecord a, b;
  bool va = W25Q128_Read(CHIP, SSW_BOOTREC_A, &a, sizeof(a)) == W25Q128_OK && brValid(&a);
  bool vb = W25Q128_Read(CHIP, SSW_BOOTREC_B, &b, sizeof(b)) == W25Q128_OK && brValid(&b);
  if (va && (!vb || a.seq > b.seq)) { br = a; brNextSlot = SSW_BOOTREC_B; }
  else if (vb) { br = b; brNextSlot = SSW_BOOTREC_A; }
  else { memset(&br, 0, sizeof(br)); br.magic = SSW_BOOTREC_MAGIC; brNextSlot = SSW_BOOTREC_A; }
}

static bool brWrite(void)
{
  static SswBootRecord w;               /* SRAM: driver source buffer */
  br.magic = SSW_BOOTREC_MAGIC;
  br.seq++;
  br.crc = SswCrc32(&br, offsetof(SswBootRecord, crc));
  w = br;
  if (W25Q128_EraseSector(CHIP, brNextSlot) != W25Q128_OK) return false;
  if (W25Q128_ProgramPage(CHIP, brNextSlot, &w, sizeof(w)) != W25Q128_OK) return false;
  SswBootRecord v;
  if (W25Q128_Read(CHIP, brNextSlot, &v, sizeof(v)) != W25Q128_OK || memcmp(&v, &w, sizeof(v)) != 0) return false;
  brNextSlot = brNextSlot == SSW_BOOTREC_A ? SSW_BOOTREC_B : SSW_BOOTREC_A;
  return true;
}

static void publishBr(void)
{
  taskENTER_CRITICAL();
  st.state = (SswBootState)br.state; st.result = (SswResult)br.result; st.result_version = br.result_version;
  st.trial_boots = br.trial_boots;
  st.backup_size = (br.backup_size == 0xFFFFFFFFU) ? 0U : br.backup_size;
  taskEXIT_CRITICAL();
}

/* ---- staging verification -------------------------------------------------- */
static const char *checkHeader(const SswHeader *h, uint32_t expectSize)
{
  if (h->magic == SSW_MAGIC_DEV) return "debug build (run tools/make_ssw.py)";
  if (h->magic != SSW_MAGIC_RELEASE) return "not a SmartSwitch image";
  if (h->header_version != SSW_HEADER_VERSION || h->header_size != SSW_HEADER_SIZE) return "unsupported header version";
  if (strncmp(h->board, SSW_BOARD, sizeof(h->board)) != 0) return "image for another board";
  if (SswCrc32(h, offsetof(SswHeader, header_crc)) != h->header_crc) return "header CRC error";
  if (h->load_addr != SSW_APP_ADDR || h->vector_offset != SSW_APP_VECTOR_OFF) return "wrong load address";
  if (h->image_end <= SSW_APP_ADDR + SSW_HEADER_SIZE || h->image_end > SSW_APP_ADDR + SSW_APP_MAX) return "bad image size";
  if (h->payload_size != h->image_end - SSW_APP_ADDR - SSW_HEADER_SIZE) return "bad payload size";
  if (expectSize && expectSize != h->image_end - SSW_APP_ADDR) return "file size does not match the header";
  return NULL;
}

static const char *verifySlot(uint32_t expectSize, SswHeader *out)
{
  static SswHeader h;
  if (W25Q128_Read(CHIP, SSW_SLOT_STAGING, &h, sizeof(h)) != W25Q128_OK) return "flash read error";
  const char *why = checkHeader(&h, expectSize);
  if (why) return why;
  SswSha256 c; uint8_t d[32];
  SswSha256_Init(&c);
  for (uint32_t off = 0; off < h.payload_size; ) {
    uint32_t n = h.payload_size - off > sizeof(rbuf) ? sizeof(rbuf) : h.payload_size - off;
    if (W25Q128_Read(CHIP, SSW_SLOT_STAGING + SSW_HEADER_SIZE + off, rbuf, n) != W25Q128_OK) return "flash read error";
    SswSha256_Update(&c, rbuf, n); off += n;
  }
  SswSha256_Final(&c, d);
  if (memcmp(d, h.sha256, 32) != 0) return "SHA-256 mismatch";
  if (out) *out = h;
  return NULL;
}

static void setSlot(FotaSlotState s, const SswHeader *h, const char *reason)
{
  taskENTER_CRITICAL();
  st.slot = s; st.slot_reason = reason;
  if (h) {
    st.slot_version = h->fw_version; memcpy(st.slot_build, h->fw_build, 32); st.slot_build[32] = 0;
    st.slot_size = h->image_end - SSW_APP_ADDR;
  } else if (s != FOTA_SLOT_RECEIVING) {
    st.slot_version = 0; st.slot_build[0] = 0; st.slot_size = 0;
  }
  taskEXIT_CRITICAL();
}

void Fota_Init(void)
{
  const SswHeader *run = (const SswHeader *)SSW_APP_ADDR;
  st.run_version = run->fw_version;
  memcpy(st.run_build, run->fw_build, 32); st.run_build[32] = 0;
  st.run_dev = run->magic == SSW_MAGIC_DEV;
  const uint32_t *blv = (const uint32_t *)SSW_BL_ADDR;
  st.bootloader_present = blv[0] > 0x20000000UL && blv[0] <= 0x20020000UL && (blv[1] & 1U) && blv[1] < SSW_APP_ADDR;
  brRead();
  publishBr();
  SswHeader h;
  const char *why = verifySlot(0, &h);
  uint32_t first;
  bool empty = W25Q128_Read(CHIP, SSW_SLOT_STAGING, &first, 4) == W25Q128_OK && first == 0xFFFFFFFFU;
  if (empty) setSlot(FOTA_SLOT_EMPTY, NULL, NULL);
  else setSlot(why ? FOTA_SLOT_INVALID : FOTA_SLOT_VALID, why ? NULL : &h, why);
  (void)DebugLog_Printf("FOTA: running %lu.%lu.%lu (%s)%s, boot %s, last %s, backup %lu B, staging %s\r\n",
      (unsigned long)(st.run_version >> 16), (unsigned long)((st.run_version >> 8) & 255U), (unsigned long)(st.run_version & 255U),
      st.run_build, st.run_dev ? " DEV" : "", Fota_StateName(st.state), Fota_ResultName(st.result),
      (unsigned long)st.backup_size, Fota_SlotName(st.slot));
  Storage_LogEvent(ST_EV_FIRMWARE, (int32_t)st.state, (int32_t)st.result, (int32_t)st.run_version, 0);
}

/* ---- upload ------------------------------------------------------------------ */
static bool flushPage(void)
{
  if (pagePos == 0U) return true;
  uint32_t addr = SSW_SLOT_STAGING + writePos;
  if ((addr % W25Q128_SECTOR_SIZE) == 0U && W25Q128_EraseSector(CHIP, addr) != W25Q128_OK) return false;
  if (W25Q128_ProgramPage(CHIP, addr, page, pagePos) != W25Q128_OK) return false;
  writePos += pagePos;
  pagePos = 0;
  return true;
}

bool Fota_UploadBegin(uint32_t size, const char **reason)
{
  if (st.reboot_pending) { *reason = "reboot pending"; return false; }
  if (size < SSW_HEADER_SIZE + 64U || size > SSW_APP_MAX) { *reason = "bad file size"; return false; }
  if (br.state != SSW_BR_IDLE && br.state != SSW_BR_TRIAL) { *reason = "update in progress"; return false; }
  total = size; writePos = 0; pagePos = 0;
  setSlot(FOTA_SLOT_RECEIVING, NULL, NULL);
  taskENTER_CRITICAL(); st.received = 0; taskEXIT_CRITICAL();
  /* invalidate the old staging image first: its first sector is erased by the first page */
  return true;
}

bool Fota_UploadWrite(const uint8_t *data, uint32_t len)
{
  while (len) {
    uint32_t n = sizeof(page) - pagePos;
    if (n > len) n = len;
    memcpy(page + pagePos, data, n);
    pagePos += n; data += n; len -= n;
    if (pagePos == sizeof(page) && !flushPage()) return false;
  }
  taskENTER_CRITICAL(); st.received = writePos + pagePos; taskEXIT_CRITICAL();
  return true;
}

void Fota_UploadAbort(const char *reason)
{
  setSlot(FOTA_SLOT_INVALID, NULL, reason);
}

bool Fota_UploadEnd(const char **reason)
{
  if (!flushPage()) { *reason = "flash write error"; Fota_UploadAbort(*reason); return false; }
  if (writePos != total) { *reason = "incomplete upload"; Fota_UploadAbort(*reason); return false; }
  SswHeader h;
  const char *why = verifySlot(total, &h);
  if (why) { *reason = why; Fota_UploadAbort(why); return false; }
  setSlot(FOTA_SLOT_VALID, &h, NULL);
  (void)DebugLog_Printf("FOTA: staging image %lu.%lu.%lu verified (%lu bytes)\r\n",
      (unsigned long)(h.fw_version >> 16), (unsigned long)((h.fw_version >> 8) & 255U), (unsigned long)(h.fw_version & 255U),
      (unsigned long)total);
  return true;
}

/* ---- install / rollback ------------------------------------------------------ */
static void scheduleReboot(void)
{
  Storage_CheckpointEnergy(Acquisition_EnergyUwh());
  taskENTER_CRITICAL();
  st.reboot_pending = true;
  rebootAt = xTaskGetTickCount() + pdMS_TO_TICKS(REBOOT_DELAY_MS);
  taskEXIT_CRITICAL();
}

bool Fota_RequestInstall(const char **reason)
{
  if (st.reboot_pending) { *reason = "reboot pending"; return false; }
  if (st.slot != FOTA_SLOT_VALID) { *reason = "no verified image in the staging slot"; return false; }
  if (!st.bootloader_present) { *reason = "no bootloader on this board"; return false; }
  if (verifySlot(st.slot_size, NULL) != NULL) { *reason = "staging image changed"; return false; }
  br.state = SSW_BR_PENDING; br.result = SSW_RES_NONE;
  if (!brWrite()) { *reason = "boot record write failed"; return false; }
  publishBr();
  Storage_LogEvent(ST_EV_FIRMWARE, SSW_BR_PENDING, 0, (int32_t)st.slot_version, 0);
  (void)DebugLog_Printf("FOTA: install requested, rebooting\r\n");
  scheduleReboot();
  return true;
}

bool Fota_RequestRollback(const char **reason)
{
  if (st.reboot_pending) { *reason = "reboot pending"; return false; }
  if (st.backup_size == 0U) { *reason = "no backup image"; return false; }
  if (!st.bootloader_present) { *reason = "no bootloader on this board"; return false; }
  br.state = SSW_BR_ROLLBACK; br.result = SSW_RES_NONE;
  if (!brWrite()) { *reason = "boot record write failed"; return false; }
  publishBr();
  Storage_LogEvent(ST_EV_FIRMWARE, SSW_BR_ROLLBACK, 0, (int32_t)st.run_version, 0);
  (void)DebugLog_Printf("FOTA: rollback requested, rebooting\r\n");
  scheduleReboot();
  return true;
}

void Fota_Tick(void)
{
  TickType_t now = xTaskGetTickCount();
  if (st.reboot_pending && (int32_t)(now - rebootAt) >= 0) {
    (void)DebugLog_Printf("FOTA: reboot\r\n");
    vTaskDelay(pdMS_TO_TICKS(100));           /* let the log drain */
    NVIC_SystemReset();
  }
  /* Confirm a trial image once the system has been healthy (Supervisor run phase, every
   * registered heartbeat fresh) for 60 s. Without this the bootloader rolls back. */
#ifndef FOTA_TEST_NO_CONFIRM
  if (!confirmDone && br.state == SSW_BR_TRIAL && now * portTICK_PERIOD_MS >= CONFIRM_AFTER_MS &&
      strcmp(Supervisor_Phase(), "run") == 0) {
    confirmDone = true;
    br.state = SSW_BR_IDLE; br.result = SSW_RES_COMMITTED; br.result_version = st.run_version;
    if (brWrite()) {
      publishBr();
      Storage_LogEvent(ST_EV_FIRMWARE, SSW_BR_IDLE, SSW_RES_COMMITTED, (int32_t)st.run_version, 0);
      (void)DebugLog_Printf("FOTA: firmware confirmed (committed)\r\n");
    } else {
      confirmDone = false;
      brRead();
    }
  }
#endif
}

void Fota_GetStatus(FotaStatus *out)
{
  taskENTER_CRITICAL();
  *out = st;
  taskEXIT_CRITICAL();
}
