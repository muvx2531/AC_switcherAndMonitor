#ifndef I2C_BUS_H
#define I2C_BUS_H

#include <stdint.h>
#include "stm32f4xx_hal.h"

typedef enum { I2C_BUS_OK = 0, I2C_BUS_INVALID, I2C_BUS_LOCK_TIMEOUT,
  I2C_BUS_START_ERROR, I2C_BUS_TRANSFER_ERROR, I2C_BUS_TIMEOUT, I2C_BUS_FAULTED } I2cBusResult;

/* Initialize once from a task after the scheduler and I2C1 start. */
I2cBusResult I2cBus_Init(I2C_HandleTypeDef *bus);
/* All accesses to this bus must use this lock, including OLED transactions. */
I2cBusResult I2cBus_Lock(void);
void I2cBus_Unlock(void);
/* Caller holds bus lock; uses 7-bit address and DMA-safe internal storage. */
I2cBusResult I2cBus_ReadRegLocked(uint8_t address, uint8_t reg, uint8_t *value);
I2cBusResult I2cBus_WriteRegLocked(uint8_t address, uint8_t reg, uint8_t value);
uint32_t I2cBus_LastHalError(void);

#endif
