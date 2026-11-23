#include "flash_probe.h"
#include "main.h"
#include "debug_log.h"
#include "cmsis_os.h"
#include <stdbool.h>
#include <stdint.h>

extern SPI_HandleTypeDef hspi2;

#define FLASH_TIMEOUT_MS 100U
#define CANDIDATE_SECTOR_ADDR 0x00FFF000UL
#define SECTOR_SIZE 4096U

static void selectFlash(uint16_t cs)
{
  HAL_GPIO_WritePin(SPI_CS1_GPIO_Port, SPI_CS1_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(SPI_CS2_GPIO_Port, SPI_CS2_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(GPIOD, cs, GPIO_PIN_RESET);
}

static void deselectFlash(uint16_t cs)
{
  HAL_GPIO_WritePin(GPIOD, cs, GPIO_PIN_SET);
}

static bool readCommand(uint16_t cs, uint8_t command, uint8_t *data, uint16_t size)
{
  selectFlash(cs);
  HAL_StatusTypeDef result = HAL_SPI_Transmit(&hspi2, &command, 1, FLASH_TIMEOUT_MS);
  if (result == HAL_OK) result = HAL_SPI_Receive(&hspi2, data, size, FLASH_TIMEOUT_MS);
  deselectFlash(cs);
  return result == HAL_OK;
}

static bool inspectSector(uint16_t cs, uint32_t *nonBlank)
{
  uint8_t command[4] = {0x03, (uint8_t)(CANDIDATE_SECTOR_ADDR >> 16),
      (uint8_t)(CANDIDATE_SECTOR_ADDR >> 8), (uint8_t)CANDIDATE_SECTOR_ADDR};
  uint8_t buffer[128];
  *nonBlank = 0;
  selectFlash(cs);
  HAL_StatusTypeDef result = HAL_SPI_Transmit(&hspi2, command, sizeof(command), FLASH_TIMEOUT_MS);
  for (unsigned int offset = 0; result == HAL_OK && offset < SECTOR_SIZE; offset += sizeof(buffer)) {
    result = HAL_SPI_Receive(&hspi2, buffer, sizeof(buffer), FLASH_TIMEOUT_MS);
    if (result == HAL_OK) {
      for (unsigned int i = 0; i < sizeof(buffer); i++) if (buffer[i] != 0xffU) (*nonBlank)++;
    }
  }
  deselectFlash(cs);
  return result == HAL_OK;
}

void FlashProbe_Run(void)
{
  const uint16_t pins[] = {SPI_CS1_Pin, SPI_CS2_Pin};
  for (unsigned int index = 0; index < 2; index++) {
    uint8_t id[3], status1, status2, status3;
    uint32_t nonBlank;
    if (!readCommand(pins[index], 0x9F, id, sizeof(id)) ||
        !readCommand(pins[index], 0x05, &status1, 1) ||
        !readCommand(pins[index], 0x35, &status2, 1) ||
        !readCommand(pins[index], 0x15, &status3, 1)) {
      (void)DebugLog_Printf("FLASH%u: SPI read failed\r\n", index + 1U);
      continue;
    }
    (void)DebugLog_Printf("FLASH%u: JEDEC=%02X %02X %02X status=%02X %02X %02X\r\n",
        index + 1U, id[0], id[1], id[2], status1, status2, status3);
    if (id[0] != 0xEFU || id[2] != 0x18U || (id[1] != 0x40U && id[1] != 0x70U)) {
      (void)DebugLog_Printf("FLASH%u: unexpected JEDEC ID; sector scan skipped\r\n", index + 1U);
      continue;
    }
    if (inspectSector(pins[index], &nonBlank)) {
      (void)DebugLog_Printf("FLASH%u: sector FFF000-FFFFFF non-FF bytes=%lu\r\n",
          index + 1U, (unsigned long)nonBlank);
    } else {
      (void)DebugLog_Printf("FLASH%u: sector read failed\r\n", index + 1U);
    }
  }
}

static bool writeEnable(uint16_t cs)
{
  uint8_t command = 0x06, status;
  selectFlash(cs);
  HAL_StatusTypeDef result = HAL_SPI_Transmit(&hspi2, &command, 1, FLASH_TIMEOUT_MS);
  deselectFlash(cs);
  return result == HAL_OK && readCommand(cs, 0x05, &status, 1) && (status & 0x02U);
}

static bool waitReady(uint16_t cs, uint32_t timeoutMs)
{
  uint32_t start = HAL_GetTick();
  uint8_t status;
  do {
    if (!readCommand(cs, 0x05, &status, 1)) return false;
    if ((status & 0x01U) == 0U) return true;
    osDelay(1);
  } while ((HAL_GetTick() - start) < timeoutMs);
  return false;
}

static bool readPage(uint16_t cs, uint8_t *data, uint16_t length)
{
  uint8_t command[] = {0x03, 0xff, 0xf0, 0x00};
  selectFlash(cs);
  HAL_StatusTypeDef result = HAL_SPI_Transmit(&hspi2, command, sizeof(command), FLASH_TIMEOUT_MS);
  if (result == HAL_OK) result = HAL_SPI_Receive(&hspi2, data, length, FLASH_TIMEOUT_MS);
  deselectFlash(cs);
  return result == HAL_OK;
}

void FlashProbe_TestBlankSectors(void)
{
  const uint16_t pins[] = {SPI_CS1_Pin, SPI_CS2_Pin};
  static uint8_t pattern[256], data[256], other[256];
  uint32_t blank[2];
  for (unsigned int i = 0; i < sizeof(pattern); i++) pattern[i] = (uint8_t)(i ^ 0xa5U);
  for (unsigned int i = 0; i < 2; i++) {
    uint8_t id[3];
    if (!readCommand(pins[i], 0x9f, id, sizeof(id)) || id[0] != 0xefU ||
        (id[1] != 0x40U && id[1] != 0x70U) || id[2] != 0x18U) {
      (void)DebugLog_Printf("FLASH TEST: skipped, invalid JEDEC ID\r\n");
      return;
    }
  }
  /* Both complete sectors must be blank before any mutation. */
  if (!inspectSector(pins[0], &blank[0]) || !inspectSector(pins[1], &blank[1]) ||
      blank[0] != 0U || blank[1] != 0U) {
    (void)DebugLog_Printf("FLASH TEST: skipped, sector not blank or read error\r\n");
    return;
  }
  for (unsigned int i = 0; i < 2; i++) {
    uint8_t command[] = {0x02, 0xff, 0xf0, 0x00};
    bool ok = writeEnable(pins[i]);
    if (ok) {
      selectFlash(pins[i]);
      HAL_StatusTypeDef result = HAL_SPI_Transmit(&hspi2, command, sizeof(command), FLASH_TIMEOUT_MS);
      if (result == HAL_OK) result = HAL_SPI_Transmit(&hspi2, pattern, sizeof(pattern), FLASH_TIMEOUT_MS);
      deselectFlash(pins[i]);
      ok = result == HAL_OK && waitReady(pins[i], 1000);
    }
    if (ok) ok = readPage(pins[i], data, sizeof(data));
    if (ok) for (unsigned int j = 0; j < sizeof(data); j++) if (data[j] != pattern[j]) {ok = false; break;}
    bool isolated = readPage(pins[1U-i], other, sizeof(other));
    if (isolated) for (unsigned int j = 0; j < sizeof(other); j++) if (other[j] != 0xffU) {isolated = false; break;}
    (void)DebugLog_Printf("FLASH%u: page program/read %s; other CS blank %s\r\n",
        i + 1U, ok ? "PASS" : "FAIL", isolated ? "PASS" : "FAIL");
    /* The sector was confirmed blank at test start; restore it after the test. */
    uint8_t erase[] = {0x20, 0xff, 0xf0, 0x00};
    bool restored = writeEnable(pins[i]);
    if (restored) {
      selectFlash(pins[i]);
      HAL_StatusTypeDef result = HAL_SPI_Transmit(&hspi2, erase, sizeof(erase), FLASH_TIMEOUT_MS);
      deselectFlash(pins[i]);
      restored = result == HAL_OK && waitReady(pins[i], 5000);
    }
    if (restored) restored = inspectSector(pins[i], &blank[i]) && blank[i] == 0U;
    (void)DebugLog_Printf("FLASH%u: sector erase/blank check %s\r\n", i + 1U, restored ? "PASS" : "FAIL");
    if (!ok || !isolated || !restored) return;
  }
}
