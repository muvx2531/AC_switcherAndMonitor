#ifndef W25Q128_H
#define W25Q128_H

#include <stddef.h>
#include <stdint.h>
#include "stm32f4xx_hal.h"

#define W25Q128_CAPACITY (16UL * 1024UL * 1024UL)
#define W25Q128_PAGE_SIZE 256U
#define W25Q128_SECTOR_SIZE 4096U

typedef enum { W25Q128_CHIP_1 = 0, W25Q128_CHIP_2 = 1 } W25Q128_Chip;
typedef enum {
  W25Q128_OK = 0, W25Q128_INVALID, W25Q128_NOT_READY,
  W25Q128_LOCK_TIMEOUT, W25Q128_DMA_ERROR, W25Q128_DMA_TIMEOUT,
  W25Q128_DEVICE_TIMEOUT, W25Q128_WRITE_DISABLED
} W25Q128_Result;

/* Call once from a task after SPI and the scheduler are started. */
W25Q128_Result W25Q128_Init(SPI_HandleTypeDef *spi);
/* Task context only. DMA moves data; calls wait for completion with a timeout. */
W25Q128_Result W25Q128_ReadId(W25Q128_Chip chip, uint8_t id[3]);
W25Q128_Result W25Q128_ReadStatus(W25Q128_Chip chip, uint8_t status[3]);
W25Q128_Result W25Q128_Read(W25Q128_Chip chip, uint32_t address, void *data, size_t length);
/* Programs one page at most; caller must ensure requested bits can go 1->0. */
W25Q128_Result W25Q128_ProgramPage(W25Q128_Chip chip, uint32_t address, const void *data, size_t length);
/* Erases a complete aligned 4 KiB sector. Data is lost. */
W25Q128_Result W25Q128_EraseSector(W25Q128_Chip chip, uint32_t address);

#endif
