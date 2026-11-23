#include "debug_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

/* This ordinary static array is linked into DMA-accessible SRAM, not CCM. */
static uint8_t txBuffer[DEBUG_LOG_BUFFER_SIZE];
static size_t head, tail, used, inFlight;
static volatile bool txComplete;
static UART_HandleTypeDef *debugUart;
static SemaphoreHandle_t bufferMutex;
static StaticSemaphore_t mutexStorage;
static TaskHandle_t txTask;
static uint32_t highWater, dropped, startErrors, transferErrors;

static void txCompleteCallback(UART_HandleTypeDef *uart);

static void txWorker(void *argument)
{
  (void)argument;
  for (;;) {
    /* Periodic wake also permits retry after a DMA start failure. */
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    if (xSemaphoreTake(bufferMutex, portMAX_DELAY) != pdTRUE) continue;

    taskENTER_CRITICAL();
    bool completed = txComplete;
    txComplete = false;
    taskEXIT_CRITICAL();
    if (inFlight != 0U && (completed || debugUart->gState == HAL_UART_STATE_READY)) {
      /* READY without a completion callback means the transfer was aborted. */
      tail = (tail + inFlight) % DEBUG_LOG_BUFFER_SIZE;
      used -= inFlight;
      inFlight = 0U;
      if (!completed) transferErrors++;
    }
    if (inFlight == 0U && used != 0U) {
      size_t count = DEBUG_LOG_BUFFER_SIZE - tail;
      if (count > used) count = used;
      inFlight = count;
      if (HAL_UART_Transmit_DMA(debugUart, &txBuffer[tail], (uint16_t)count) != HAL_OK) {
        inFlight = 0U;
        startErrors++;
      }
    }
    xSemaphoreGive(bufferMutex);
  }
}

bool DebugLog_Init(UART_HandleTypeDef *uart)
{
  if (uart == NULL || __get_IPSR() != 0U ||
      xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) return false;
  if (txTask != NULL) return uart == debugUart;
  /* Per-handle callback (USE_HAL_UART_REGISTER_CALLBACKS): this module does not
   * define the global HAL_UART_TxCpltCallback. HAL accepts registration only
   * while the UART TX state is READY. */
  if (HAL_UART_RegisterCallback(uart, HAL_UART_TX_COMPLETE_CB_ID, txCompleteCallback) != HAL_OK)
    return false;
  debugUart = uart;
  if (bufferMutex == NULL) bufferMutex = xSemaphoreCreateMutexStatic(&mutexStorage);
  if (bufferMutex == NULL) return false;
  if (xTaskCreate(txWorker, "debugTx", 256U, NULL, tskIDLE_PRIORITY + 2U, &txTask) != pdPASS) {
    txTask = NULL;
    return false;
  }
  return true;
}

bool DebugLog_Write(const void *data, size_t length)
{
  if (data == NULL || length == 0U || length > DEBUG_LOG_MESSAGE_MAX ||
      txTask == NULL || __get_IPSR() != 0U) return false;
  if (xSemaphoreTake(bufferMutex, 0) != pdTRUE) {
    taskENTER_CRITICAL(); dropped++; taskEXIT_CRITICAL();
    return false;
  }
  if (length > DEBUG_LOG_BUFFER_SIZE - used) {
    taskENTER_CRITICAL(); dropped++; taskEXIT_CRITICAL();
    xSemaphoreGive(bufferMutex);
    return false;
  }
  size_t first = DEBUG_LOG_BUFFER_SIZE - head;
  if (first > length) first = length;
  memcpy(&txBuffer[head], data, first);
  memcpy(txBuffer, (const uint8_t *)data + first, length - first);
  head = (head + length) % DEBUG_LOG_BUFFER_SIZE;
  used += length;
  if (used > highWater) highWater = (uint32_t)used;
  xSemaphoreGive(bufferMutex);
  xTaskNotifyGive(txTask);
  return true;
}

bool DebugLog_Printf(const char *format, ...)
{
  if (format == NULL || __get_IPSR() != 0U || txTask == NULL) return false;
  char message[DEBUG_LOG_MESSAGE_MAX];
  uint32_t ms = (uint32_t)(((uint64_t)xTaskGetTickCount() * 1000U) / configTICK_RATE_HZ);
  int prefix = snprintf(message, sizeof(message), "[%lu ms] ", (unsigned long)ms);
  if (prefix < 0 || (size_t)prefix >= sizeof(message)) return false;
  va_list args;
  va_start(args, format);
  int count = vsnprintf(message + prefix, sizeof(message) - (size_t)prefix, format, args);
  va_end(args);
  if (count < 0 || (size_t)count >= sizeof(message) - (size_t)prefix) {
    taskENTER_CRITICAL(); dropped++; taskEXIT_CRITICAL();
    return false;
  }
  return DebugLog_Write(message, (size_t)prefix + (size_t)count);
}

bool DebugLog_GetStats(DebugLogStats *stats)
{
  if (stats == NULL || txTask == NULL || __get_IPSR() != 0U ||
      xSemaphoreTake(bufferMutex, 0) != pdTRUE) return false;
  stats->queued_bytes = (uint32_t)used;
  stats->high_water_bytes = highWater;
  taskENTER_CRITICAL(); stats->dropped_messages = dropped; taskEXIT_CRITICAL();
  stats->dma_start_errors = startErrors;
  stats->dma_transfer_errors = transferErrors;
  xSemaphoreGive(bufferMutex);
  return true;
}

static void txCompleteCallback(UART_HandleTypeDef *uart)
{
  if (uart != debugUart || txTask == NULL) return;
  /* The worker owns the ring; the ISR only records and signals completion. */
  BaseType_t wake = pdFALSE;
  txComplete = true;
  vTaskNotifyGiveFromISR(txTask, &wake);
  portYIELD_FROM_ISR(wake);
}
