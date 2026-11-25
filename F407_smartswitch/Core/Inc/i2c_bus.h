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
/* Block write after a 1-byte register/control byte (OLED). len 1..I2C_BUS_BLOCK_MAX; data is
 * copied into internal DMA storage. Release the lock between blocks so Control can get in. */
#define I2C_BUS_BLOCK_MAX 128U
I2cBusResult I2cBus_WriteBlockLocked(uint8_t address, uint8_t reg, const uint8_t *data, uint16_t len);
uint32_t I2cBus_LastHalError(void);
/* Bus recovery: 9 SCL clocks + STOP, then re-init I2C1/DMA/IRQ and callbacks.
 * Writes no slave register. Run once at boot before the first transfer (S6-1);
 * I2cBus_Lock also runs it automatically (max 1/s) after a timeout fault. */
I2cBusResult I2cBus_Recover(void);
uint32_t I2cBus_RecoveryCount(void);

#endif
