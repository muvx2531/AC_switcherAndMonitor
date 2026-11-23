#include "w25q128.h"
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <stdbool.h>
#include <string.h>

#define DMA_TIMEOUT_MS 100U
#define LOCK_TIMEOUT_MS 6000U
#define PROGRAM_TIMEOUT_MS 1000U
#define ERASE_TIMEOUT_MS 5000U

static SPI_HandleTypeDef *bus;
static StaticSemaphore_t mutexStorage, doneStorage;
static SemaphoreHandle_t busMutex, dmaDone;
static uint8_t tx[4 + W25Q128_PAGE_SIZE], rx[4 + W25Q128_PAGE_SIZE];
static volatile bool transferActive, transferError;

static void txRxCompleteCallback(SPI_HandleTypeDef *spi);
static void errorCallback(SPI_HandleTypeDef *spi);

static bool validChip(W25Q128_Chip chip) { return chip == W25Q128_CHIP_1 || chip == W25Q128_CHIP_2; }
static bool validRange(uint32_t address, size_t length)
{
  return length > 0U && address < W25Q128_CAPACITY && length <= W25Q128_CAPACITY - address;
}
static void selectChip(W25Q128_Chip chip, GPIO_PinState state)
{
  if (chip == W25Q128_CHIP_1) HAL_GPIO_WritePin(SPI_CS1_GPIO_Port, SPI_CS1_Pin, state);
  else HAL_GPIO_WritePin(SPI_CS2_GPIO_Port, SPI_CS2_Pin, state);
}
static W25Q128_Result lockBus(W25Q128_Chip chip)
{
  if (!validChip(chip) || __get_IPSR() != 0U) return W25Q128_INVALID;
  if (bus == NULL || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) return W25Q128_NOT_READY;
  return xSemaphoreTake(busMutex, pdMS_TO_TICKS(LOCK_TIMEOUT_MS)) == pdTRUE ? W25Q128_OK : W25Q128_LOCK_TIMEOUT;
}
static W25Q128_Result transfer(W25Q128_Chip chip, uint16_t count)
{
  while (xSemaphoreTake(dmaDone, 0) == pdTRUE) { }
  transferError = false;
  transferActive = true;
  selectChip(chip, GPIO_PIN_RESET);
  if (HAL_SPI_TransmitReceive_DMA(bus, tx, rx, count) != HAL_OK) {
    transferActive = false;
    selectChip(chip, GPIO_PIN_SET);
    return W25Q128_DMA_ERROR;
  }
  if (xSemaphoreTake(dmaDone, pdMS_TO_TICKS(DMA_TIMEOUT_MS)) != pdTRUE) {
    taskENTER_CRITICAL(); transferActive = false; taskEXIT_CRITICAL();
    (void)HAL_SPI_Abort(bus);
    selectChip(chip, GPIO_PIN_SET);
    return W25Q128_DMA_TIMEOUT;
  }
  transferActive = false;
  selectChip(chip, GPIO_PIN_SET);
  return transferError ? W25Q128_DMA_ERROR : W25Q128_OK;
}
static W25Q128_Result statusLocked(W25Q128_Chip chip, uint8_t command, uint8_t *status)
{
  tx[0] = command; tx[1] = 0xff;
  W25Q128_Result r = transfer(chip, 2);
  if (r == W25Q128_OK) *status = rx[1];
  return r;
}
static W25Q128_Result waitReady(W25Q128_Chip chip, uint32_t timeout)
{
  TickType_t start = xTaskGetTickCount();
  do {
    uint8_t status;
    W25Q128_Result r = statusLocked(chip, 0x05, &status);
    if (r != W25Q128_OK) return r;
    if ((status & 1U) == 0U) return W25Q128_OK;
    vTaskDelay(pdMS_TO_TICKS(1));
  } while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(timeout));
  return W25Q128_DEVICE_TIMEOUT;
}
static W25Q128_Result writeEnable(W25Q128_Chip chip)
{
  uint8_t status;
  tx[0] = 0x06;
  W25Q128_Result r = transfer(chip, 1);
  if (r == W25Q128_OK) r = statusLocked(chip, 0x05, &status);
  if (r == W25Q128_OK && (status & 2U) == 0U) r = W25Q128_WRITE_DISABLED;
  return r;
}
static void addressCommand(uint8_t command, uint32_t address)
{
  tx[0] = command; tx[1] = (uint8_t)(address >> 16);
  tx[2] = (uint8_t)(address >> 8); tx[3] = (uint8_t)address;
}

W25Q128_Result W25Q128_Init(SPI_HandleTypeDef *spi)
{
  if (!spi || spi->Instance != SPI2 || __get_IPSR() != 0U ||
      xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) return W25Q128_INVALID;
  if (bus != NULL) return bus == spi ? W25Q128_OK : W25Q128_INVALID;
  /* Per-handle callbacks (USE_HAL_SPI_REGISTER_CALLBACKS); HAL requires the
   * handle to be READY, which holds before the first transfer. */
  if (HAL_SPI_RegisterCallback(spi, HAL_SPI_TX_RX_COMPLETE_CB_ID, txRxCompleteCallback) != HAL_OK ||
      HAL_SPI_RegisterCallback(spi, HAL_SPI_ERROR_CB_ID, errorCallback) != HAL_OK)
    return W25Q128_NOT_READY;
  if (busMutex == NULL) busMutex = xSemaphoreCreateMutexStatic(&mutexStorage);
  if (dmaDone == NULL) dmaDone = xSemaphoreCreateBinaryStatic(&doneStorage);
  if (busMutex == NULL || dmaDone == NULL) return W25Q128_NOT_READY;
  HAL_GPIO_WritePin(SPI_CS1_GPIO_Port, SPI_CS1_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(SPI_CS2_GPIO_Port, SPI_CS2_Pin, GPIO_PIN_SET);
  bus = spi;
  return W25Q128_OK;
}

W25Q128_Result W25Q128_ReadId(W25Q128_Chip chip, uint8_t id[3])
{
  if (!id) return W25Q128_INVALID;
  W25Q128_Result r = lockBus(chip); if (r != W25Q128_OK) return r;
  tx[0] = 0x9f; memset(tx + 1, 0xff, 3);
  r = transfer(chip, 4); if (r == W25Q128_OK) memcpy(id, rx + 1, 3);
  xSemaphoreGive(busMutex); return r;
}
W25Q128_Result W25Q128_ReadStatus(W25Q128_Chip chip, uint8_t status[3])
{
  if (!status) return W25Q128_INVALID;
  W25Q128_Result r = lockBus(chip); if (r != W25Q128_OK) return r;
  r = statusLocked(chip, 0x05, &status[0]);
  if (r == W25Q128_OK) r = statusLocked(chip, 0x35, &status[1]);
  if (r == W25Q128_OK) r = statusLocked(chip, 0x15, &status[2]);
  xSemaphoreGive(busMutex); return r;
}
W25Q128_Result W25Q128_Read(W25Q128_Chip chip, uint32_t address, void *data, size_t length)
{
  if (!data || !validRange(address, length)) return W25Q128_INVALID;
  W25Q128_Result r = lockBus(chip); if (r != W25Q128_OK) return r;
  uint8_t *out = data;
  while (length > 0U) {
    size_t n = length > W25Q128_PAGE_SIZE ? W25Q128_PAGE_SIZE : length;
    addressCommand(0x03, address); memset(tx + 4, 0xff, n);
    r = transfer(chip, (uint16_t)(n + 4)); if (r != W25Q128_OK) break;
    memcpy(out, rx + 4, n); out += n; address += n; length -= n;
  }
  xSemaphoreGive(busMutex); return r;
}
W25Q128_Result W25Q128_ProgramPage(W25Q128_Chip chip, uint32_t address, const void *data, size_t length)
{
  if (!data || !validRange(address, length) || length > W25Q128_PAGE_SIZE ||
      (address % W25Q128_PAGE_SIZE) + length > W25Q128_PAGE_SIZE) return W25Q128_INVALID;
  W25Q128_Result r = lockBus(chip); if (r != W25Q128_OK) return r;
  r = waitReady(chip, PROGRAM_TIMEOUT_MS);
  if (r == W25Q128_OK) r = writeEnable(chip);
  if (r == W25Q128_OK) { addressCommand(0x02, address); memcpy(tx + 4, data, length); r = transfer(chip, (uint16_t)(length + 4)); }
  if (r == W25Q128_OK) r = waitReady(chip, PROGRAM_TIMEOUT_MS);
  xSemaphoreGive(busMutex); return r;
}
W25Q128_Result W25Q128_EraseSector(W25Q128_Chip chip, uint32_t address)
{
  if (address >= W25Q128_CAPACITY || address % W25Q128_SECTOR_SIZE != 0U) return W25Q128_INVALID;
  W25Q128_Result r = lockBus(chip); if (r != W25Q128_OK) return r;
  r = waitReady(chip, ERASE_TIMEOUT_MS);
  if (r == W25Q128_OK) r = writeEnable(chip);
  if (r == W25Q128_OK) { addressCommand(0x20, address); r = transfer(chip, 4); }
  if (r == W25Q128_OK) r = waitReady(chip, ERASE_TIMEOUT_MS);
  xSemaphoreGive(busMutex); return r;
}

static void dmaSignal(SPI_HandleTypeDef *spi, bool error)
{
  if (spi != bus || !transferActive) return;
  BaseType_t wake = pdFALSE;
  transferError = error;
  xSemaphoreGiveFromISR(dmaDone, &wake);
  portYIELD_FROM_ISR(wake);
}
static void txRxCompleteCallback(SPI_HandleTypeDef *spi) { dmaSignal(spi, false); }
static void errorCallback(SPI_HandleTypeDef *spi) { dmaSignal(spi, true); }
