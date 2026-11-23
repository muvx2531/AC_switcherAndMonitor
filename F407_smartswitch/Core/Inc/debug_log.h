#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "stm32f4xx_hal.h"

#define DEBUG_LOG_BUFFER_SIZE 10240U
#define DEBUG_LOG_MESSAGE_MAX 256U

typedef struct {
  uint32_t queued_bytes;
  uint32_t high_water_bytes;
  uint32_t dropped_messages;
  uint32_t dma_start_errors;
  uint32_t dma_transfer_errors;
} DebugLogStats;

/* Initialize once from a task after the scheduler and USART DMA are running. */
bool DebugLog_Init(UART_HandleTypeDef *uart);
/* Task context only. Copies a complete message or drops it without waiting. */
bool DebugLog_Write(const void *data, size_t length);
/* Prefix with the RTOS tick in milliseconds; long messages are dropped. */
bool DebugLog_Printf(const char *format, ...);
bool DebugLog_GetStats(DebugLogStats *stats);

#endif
