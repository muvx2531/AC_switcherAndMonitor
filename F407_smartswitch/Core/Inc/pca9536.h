#ifndef PCA9536_H
#define PCA9536_H

#include <stdint.h>
#include "i2c_bus.h"

#define PCA9536_ADDRESS_7BIT 0x41U
typedef struct { uint8_t input, output, polarity, config; } PCA9536_Registers;

/* Read-only, task context. No relay state is changed during init/read. */
I2cBusResult PCA9536_ReadRegisters(PCA9536_Registers *regs);
/* Keep existing outputs; preload input pins low and change them to outputs. */
I2cBusResult PCA9536_InitOutputsKeepState(PCA9536_Registers *before,
                                         PCA9536_Registers *after);
/* Explicit output latch and directions, with read-back; may change hardware. */
I2cBusResult PCA9536_Configure(uint8_t initialOutputs, uint8_t inputMask);
/* Atomic read-modify-write on the output latch; only mask bits 0..3 change. */
I2cBusResult PCA9536_WriteMasked(uint8_t mask, uint8_t value);

#endif
