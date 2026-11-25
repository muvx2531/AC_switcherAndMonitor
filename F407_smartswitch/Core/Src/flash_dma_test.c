#include "flash_dma_test.h"
#include "w25q128.h"
#include "debug_log.h"
#include <stdbool.h>
#include <string.h>

#define TEST_SECTOR 0xFFF000UL
static uint8_t pattern[256], verify[256];

static bool identify(W25Q128_Chip chip)
{
  uint8_t id[3], status[3];
  W25Q128_Result r = W25Q128_ReadId(chip, id);
  if (r == W25Q128_OK) r = W25Q128_ReadStatus(chip, status);
  if (r != W25Q128_OK) {
    (void)DebugLog_Printf("FLASH DMA %u: identify failed %u\r\n", chip + 1U, r);
    return false;
  }
  (void)DebugLog_Printf("FLASH DMA %u: JEDEC=%02X %02X %02X status=%02X %02X %02X\r\n",
      chip + 1U, id[0], id[1], id[2], status[0], status[1], status[2]);
  return id[0] == 0xefU && (id[1] == 0x40U || id[1] == 0x70U) && id[2] == 0x18U;
}
static bool sectorBlank(W25Q128_Chip chip)
{
  for (uint32_t offset = 0; offset < W25Q128_SECTOR_SIZE; offset += sizeof(verify)) {
    if (W25Q128_Read(chip, TEST_SECTOR + offset, verify, sizeof(verify)) != W25Q128_OK) return false;
    for (size_t i = 0; i < sizeof(verify); i++) if (verify[i] != 0xffU) return false;
  }
  return true;
}
void FlashDmaTest_Identify(void)
{
  (void)identify(W25Q128_CHIP_1);
  (void)identify(W25Q128_CHIP_2);
}
void FlashDmaTest_Run(void)
{
  if (!identify(W25Q128_CHIP_1) || !identify(W25Q128_CHIP_2) ||
      !sectorBlank(W25Q128_CHIP_1) || !sectorBlank(W25Q128_CHIP_2)) {
    (void)DebugLog_Printf("FLASH DMA test: invalid ID or sector not blank; skipped\r\n");
    return;
  }
  if (W25Q128_ProgramPage(W25Q128_CHIP_1, TEST_SECTOR + 255U, pattern, 2) != W25Q128_INVALID ||
      W25Q128_EraseSector(W25Q128_CHIP_1, TEST_SECTOR + 1U) != W25Q128_INVALID ||
      W25Q128_Read(W25Q128_CHIP_1, W25Q128_CAPACITY - 1U, verify, 2) != W25Q128_INVALID) {
    (void)DebugLog_Printf("FLASH DMA test: bounds check FAILED\r\n");
    return;
  }
  (void)DebugLog_Printf("FLASH DMA test: bounds checks PASS\r\n");
  for (size_t j = 0; j < sizeof(pattern); j++) pattern[j] = (uint8_t)(j ^ 0xa5U);
  for (unsigned int i = 0; i < 2; i++) {
    W25Q128_Chip chip = (W25Q128_Chip)i;
    W25Q128_Chip other = (W25Q128_Chip)(1U - i);
    W25Q128_Result r = W25Q128_ProgramPage(chip, TEST_SECTOR, pattern, sizeof(pattern));
    if (r == W25Q128_OK) r = W25Q128_Read(chip, TEST_SECTOR, verify, sizeof(verify));
    bool same = r == W25Q128_OK && memcmp(pattern, verify, sizeof(pattern)) == 0;
    bool isolated = sectorBlank(other);
    (void)DebugLog_Printf("FLASH DMA %u: program/read %s, other chip blank %s (result=%u)\r\n",
        i + 1U, same ? "PASS" : "FAIL", isolated ? "PASS" : "FAIL", r);
    /* Never erase if program failed: a timeout can leave the part busy. */
    if (!same || !isolated) return;
    r = W25Q128_EraseSector(chip, TEST_SECTOR);
    bool blank = r == W25Q128_OK && sectorBlank(chip);
    (void)DebugLog_Printf("FLASH DMA %u: erase/blank %s (result=%u)\r\n",
        i + 1U, blank ? "PASS" : "FAIL", r);
    if (!blank) return;
  }
}

/* Read-only blank check of both whole chips (Phase 5a prerequisite: never assume the
 * storage is empty). Reports non-blank 64 KiB blocks. Calls Supervisor_Heartbeat. */
#include "supervisor.h"
void FlashDmaTest_Scan(void)
{
  static uint8_t buf[1024];
  for (unsigned int c = 0; c < 2U; c++) {
    W25Q128_Chip chip = (W25Q128_Chip)c;
    uint32_t dirty = 0, reported = 0;
    (void)DebugLog_Printf("FLSCAN %u: start\r\n", c + 1U);
    for (uint32_t block = 0; block < W25Q128_CAPACITY; block += 0x10000UL) {
      bool blank = true, err = false;
      for (uint32_t off = 0; off < 0x10000UL; off += sizeof(buf)) {
        if (W25Q128_Read(chip, block + off, buf, sizeof(buf)) != W25Q128_OK) { err = true; break; }
        for (size_t i = 0; i < sizeof(buf); i++) if (buf[i] != 0xffU) { blank = false; break; }
        if (!blank) break;
      }
      Supervisor_Heartbeat();
      if (err) { (void)DebugLog_Printf("FLSCAN %u: read error at 0x%06lX\r\n", c + 1U, (unsigned long)block); break; }
      if (!blank) {
        dirty++;
        if (reported++ < 24U) {
          (void)W25Q128_Read(chip, block, buf, 16);
          (void)DebugLog_Printf("FLSCAN %u: block 0x%06lX not blank, first bytes %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
              c + 1U, (unsigned long)block, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
        }
      }
    }
    (void)DebugLog_Printf("FLSCAN %u: done, %lu of 256 blocks not blank\r\n", c + 1U, (unsigned long)dirty);
  }
}
