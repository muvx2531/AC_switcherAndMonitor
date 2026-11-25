#include "i2c_bus.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <stdbool.h>

static I2C_HandleTypeDef *i2c;
static StaticSemaphore_t mutexStorage, doneStorage;
static SemaphoreHandle_t mutex, done;
static uint8_t dmaByte;
static volatile bool active, failed, faulted;
static volatile uint32_t lastError;
static uint32_t recoveries;
static TickType_t lastRecoveryTick;

#define I2C_BUS_PORT GPIOB
#define I2C_BUS_SCL_PIN GPIO_PIN_6
#define I2C_BUS_SDA_PIN GPIO_PIN_7

static void memRxCompleteCallback(I2C_HandleTypeDef *handle);
static void memTxCompleteCallback(I2C_HandleTypeDef *handle);
static void errorCallback(I2C_HandleTypeDef *handle);
static bool registerCallbacks(I2C_HandleTypeDef *handle);

I2cBusResult I2cBus_Init(I2C_HandleTypeDef *handle)
{
  if (!handle || handle->Instance != I2C1 || __get_IPSR() ||
      xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) return I2C_BUS_INVALID;
  if (i2c) return i2c == handle ? I2C_BUS_OK : I2C_BUS_INVALID;
  /* Per-handle callbacks (USE_HAL_I2C_REGISTER_CALLBACKS); HAL requires the
   * handle to be READY, which holds before the first transfer. */
  if (!registerCallbacks(handle)) return I2C_BUS_INVALID;
  if (!mutex) mutex = xSemaphoreCreateMutexStatic(&mutexStorage);
  if (!done) done = xSemaphoreCreateBinaryStatic(&doneStorage);
  if (!mutex || !done) return I2C_BUS_INVALID;
  i2c = handle;
  return I2C_BUS_OK;
}

static bool registerCallbacks(I2C_HandleTypeDef *handle)
{
  return HAL_I2C_RegisterCallback(handle, HAL_I2C_MEM_RX_COMPLETE_CB_ID, memRxCompleteCallback) == HAL_OK &&
         HAL_I2C_RegisterCallback(handle, HAL_I2C_MEM_TX_COMPLETE_CB_ID, memTxCompleteCallback) == HAL_OK &&
         HAL_I2C_RegisterCallback(handle, HAL_I2C_ERROR_CB_ID, errorCallback) == HAL_OK;
}

/* ~5 us at 168 MHz; slower is harmless for a bus recovery. */
static void busDelay(void) { for (volatile uint32_t n = 0; n < 200U; n++) { } }

/* Bus recovery (decision S3-2). Caller holds the mutex. Clocks SCL nine times so
 * a slave left mid-byte releases SDA, sends STOP, then re-initializes I2C1, its
 * DMA and IRQs. No register of any slave is written, so the PCA9536 output latch
 * is not changed (invariant I3). DeInit/Init resets HAL callbacks: re-register. */
static I2cBusResult recoverLocked(void)
{
  __HAL_I2C_DISABLE_IT(i2c, I2C_IT_EVT | I2C_IT_BUF | I2C_IT_ERR);
  CLEAR_BIT(i2c->Instance->CR2, I2C_CR2_DMAEN);
  (void)HAL_DMA_Abort(i2c->hdmatx);
  (void)HAL_DMA_Abort(i2c->hdmarx);
  SET_BIT(i2c->Instance->CR1, I2C_CR1_SWRST);
  CLEAR_BIT(i2c->Instance->CR1, I2C_CR1_SWRST);
  (void)HAL_I2C_DeInit(i2c);

  __HAL_RCC_GPIOB_CLK_ENABLE();
  GPIO_InitTypeDef gpio = {0};
  gpio.Pin = I2C_BUS_SCL_PIN | I2C_BUS_SDA_PIN;
  gpio.Mode = GPIO_MODE_OUTPUT_OD;
  gpio.Pull = GPIO_NOPULL;            /* board pull-ups */
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_WritePin(I2C_BUS_PORT, I2C_BUS_SCL_PIN | I2C_BUS_SDA_PIN, GPIO_PIN_SET);
  HAL_GPIO_Init(I2C_BUS_PORT, &gpio);
  busDelay();
  for (unsigned int i = 0; i < 9U; i++) {
    HAL_GPIO_WritePin(I2C_BUS_PORT, I2C_BUS_SCL_PIN, GPIO_PIN_RESET); busDelay();
    HAL_GPIO_WritePin(I2C_BUS_PORT, I2C_BUS_SCL_PIN, GPIO_PIN_SET); busDelay();
  }
  /* STOP: SDA low -> high while SCL is high. */
  HAL_GPIO_WritePin(I2C_BUS_PORT, I2C_BUS_SCL_PIN, GPIO_PIN_RESET); busDelay();
  HAL_GPIO_WritePin(I2C_BUS_PORT, I2C_BUS_SDA_PIN, GPIO_PIN_RESET); busDelay();
  HAL_GPIO_WritePin(I2C_BUS_PORT, I2C_BUS_SCL_PIN, GPIO_PIN_SET); busDelay();
  HAL_GPIO_WritePin(I2C_BUS_PORT, I2C_BUS_SDA_PIN, GPIO_PIN_SET); busDelay();
  bool lines = HAL_GPIO_ReadPin(I2C_BUS_PORT, I2C_BUS_SDA_PIN) == GPIO_PIN_SET &&
               HAL_GPIO_ReadPin(I2C_BUS_PORT, I2C_BUS_SCL_PIN) == GPIO_PIN_SET;
  HAL_GPIO_DeInit(I2C_BUS_PORT, I2C_BUS_SCL_PIN | I2C_BUS_SDA_PIN);

  bool ok = HAL_I2C_Init(i2c) == HAL_OK && registerCallbacks(i2c) && lines;
  recoveries++;
  lastRecoveryTick = xTaskGetTickCount();
  faulted = !ok;
  return ok ? I2C_BUS_OK : I2C_BUS_FAULTED;
}

I2cBusResult I2cBus_Recover(void)
{
  if (!i2c || __get_IPSR() || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) return I2C_BUS_INVALID;
  if (xSemaphoreTake(mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return I2C_BUS_LOCK_TIMEOUT;
  I2cBusResult r = recoverLocked();
  xSemaphoreGive(mutex);
  return r;
}

I2cBusResult I2cBus_Lock(void)
{
  if (!i2c || __get_IPSR() || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) return I2C_BUS_INVALID;
  if (xSemaphoreTake(mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return I2C_BUS_LOCK_TIMEOUT;
  /* A faulted bus is recovered automatically, at most once per second. */
  if (faulted && xTaskGetTickCount() - lastRecoveryTick >= pdMS_TO_TICKS(1000)) (void)recoverLocked();
  if (faulted) { xSemaphoreGive(mutex); return I2C_BUS_FAULTED; }
  return I2C_BUS_OK;
}
void I2cBus_Unlock(void) { (void)xSemaphoreGive(mutex); }
uint32_t I2cBus_LastHalError(void) { return lastError; }
uint32_t I2cBus_RecoveryCount(void) { return recoveries; }

static I2cBusResult transfer(uint8_t address, uint8_t reg, uint8_t *value, bool write)
{
  if (address > 0x7fU || !value) return I2C_BUS_INVALID;
  while (xSemaphoreTake(done, 0) == pdTRUE) { }
  failed = false;
  lastError = 0;
  if (write) dmaByte = *value;
  active = true;
  HAL_StatusTypeDef s = write ? HAL_I2C_Mem_Write_DMA(i2c, (uint16_t)address << 1,
      reg, I2C_MEMADD_SIZE_8BIT, &dmaByte, 1) :
      HAL_I2C_Mem_Read_DMA(i2c, (uint16_t)address << 1, reg,
      I2C_MEMADD_SIZE_8BIT, &dmaByte, 1);
  if (s != HAL_OK) {
    /* HAL refused to start (e.g. BUSY flag stuck after a glitch, handle not READY): the
     * bus needs a recovery, otherwise every later transfer fails the same way. */
    taskENTER_CRITICAL(); active = false; faulted = true; taskEXIT_CRITICAL();
    lastError = HAL_I2C_GetError(i2c);
    return I2C_BUS_START_ERROR;
  }
  if (xSemaphoreTake(done, pdMS_TO_TICKS(200)) != pdTRUE) {
    taskENTER_CRITICAL(); active = false; faulted = true; taskEXIT_CRITICAL();
    /* State is uncertain; stop DMA. The next I2cBus_Lock runs a bus recovery. */
    __HAL_I2C_DISABLE_IT(i2c, I2C_IT_EVT | I2C_IT_BUF | I2C_IT_ERR);
    CLEAR_BIT(i2c->Instance->CR2, I2C_CR2_DMAEN);
    (void)HAL_DMA_Abort(i2c->hdmatx);
    (void)HAL_DMA_Abort(i2c->hdmarx);
    lastError = HAL_I2C_GetError(i2c);
    return I2C_BUS_TIMEOUT;
  }
  active = false;
  if (failed) return I2C_BUS_TRANSFER_ERROR;
  if (!write) *value = dmaByte;
  return I2C_BUS_OK;
}
I2cBusResult I2cBus_ReadRegLocked(uint8_t address, uint8_t reg, uint8_t *value)
{ return transfer(address, reg, value, false); }
I2cBusResult I2cBus_WriteRegLocked(uint8_t address, uint8_t reg, uint8_t value)
{ return transfer(address, reg, &value, true); }

static void signalDone(I2C_HandleTypeDef *handle, bool error)
{
  if (handle != i2c || !active) return;
  BaseType_t wake = pdFALSE;
  failed = error;
  if (error) lastError = HAL_I2C_GetError(handle);
  xSemaphoreGiveFromISR(done, &wake);
  portYIELD_FROM_ISR(wake);
}
static void memRxCompleteCallback(I2C_HandleTypeDef *handle) { signalDone(handle, false); }
static void memTxCompleteCallback(I2C_HandleTypeDef *handle) { signalDone(handle, false); }
static void errorCallback(I2C_HandleTypeDef *handle) { signalDone(handle, true); }
