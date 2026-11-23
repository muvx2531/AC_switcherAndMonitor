#include "pca9536.h"

#define REG_INPUT 0U
#define REG_OUTPUT 1U
#define REG_POLARITY 2U
#define REG_CONFIG 3U
#define GPIO_MASK 0x0fU

static I2cBusResult readAllLocked(PCA9536_Registers *regs)
{
  I2cBusResult r = I2cBus_ReadRegLocked(PCA9536_ADDRESS_7BIT, REG_INPUT, &regs->input);
  if (!r) r = I2cBus_ReadRegLocked(PCA9536_ADDRESS_7BIT, REG_OUTPUT, &regs->output);
  if (!r) r = I2cBus_ReadRegLocked(PCA9536_ADDRESS_7BIT, REG_POLARITY, &regs->polarity);
  if (!r) r = I2cBus_ReadRegLocked(PCA9536_ADDRESS_7BIT, REG_CONFIG, &regs->config);
  return r;
}

I2cBusResult PCA9536_ReadRegisters(PCA9536_Registers *regs)
{
  if (!regs) return I2C_BUS_INVALID;
  I2cBusResult r = I2cBus_Lock(); if (r != I2C_BUS_OK) return r;
  r = readAllLocked(regs);
  I2cBus_Unlock(); return r;
}
I2cBusResult PCA9536_InitOutputsKeepState(PCA9536_Registers *before, PCA9536_Registers *after)
{
  if (!before || !after) return I2C_BUS_INVALID;
  I2cBusResult r = I2cBus_Lock(); if (r != I2C_BUS_OK) return r;
  r = readAllLocked(before);
  if (r == I2C_BUS_OK) {
    uint8_t inputs = before->config & GPIO_MASK;
    uint8_t outputs = (before->output & GPIO_MASK) & (uint8_t)~inputs;
    uint8_t verify;
    if (inputs != 0U) {
      /* Existing outputs keep their latch; input latches are set LOW first. */
      r = I2cBus_WriteRegLocked(PCA9536_ADDRESS_7BIT, REG_OUTPUT, outputs);
      if (!r) r = I2cBus_ReadRegLocked(PCA9536_ADDRESS_7BIT, REG_OUTPUT, &verify);
      if (!r && (verify & GPIO_MASK) != outputs) r = I2C_BUS_TRANSFER_ERROR;
      if (!r) r = I2cBus_WriteRegLocked(PCA9536_ADDRESS_7BIT, REG_CONFIG, 0U);
    }
    if (!r) r = readAllLocked(after);
    if (!r && ((after->config & GPIO_MASK) != 0U ||
        (after->output & GPIO_MASK) != outputs)) r = I2C_BUS_TRANSFER_ERROR;
  }
  I2cBus_Unlock(); return r;
}
I2cBusResult PCA9536_Configure(uint8_t initialOutputs, uint8_t inputMask)
{
  if ((initialOutputs | inputMask) & ~GPIO_MASK) return I2C_BUS_INVALID;
  I2cBusResult r = I2cBus_Lock(); if (r != I2C_BUS_OK) return r;
  uint8_t value;
  r = I2cBus_WriteRegLocked(PCA9536_ADDRESS_7BIT, REG_OUTPUT, initialOutputs);
  if (!r) r = I2cBus_ReadRegLocked(PCA9536_ADDRESS_7BIT, REG_OUTPUT, &value);
  if (!r && (value & GPIO_MASK) != initialOutputs) r = I2C_BUS_TRANSFER_ERROR;
  if (!r) r = I2cBus_WriteRegLocked(PCA9536_ADDRESS_7BIT, REG_CONFIG, inputMask);
  if (!r) r = I2cBus_ReadRegLocked(PCA9536_ADDRESS_7BIT, REG_CONFIG, &value);
  if (!r && (value & GPIO_MASK) != inputMask) r = I2C_BUS_TRANSFER_ERROR;
  I2cBus_Unlock(); return r;
}
I2cBusResult PCA9536_WriteMasked(uint8_t mask, uint8_t value)
{
  if (((mask | value) & ~GPIO_MASK) || mask == 0U) return I2C_BUS_INVALID;
  I2cBusResult r = I2cBus_Lock(); if (r != I2C_BUS_OK) return r;
  uint8_t old = 0, verify;
  r = I2cBus_ReadRegLocked(PCA9536_ADDRESS_7BIT, REG_OUTPUT, &old);
  uint8_t next = (uint8_t)((old & ~mask) | (value & mask));
  if (!r) r = I2cBus_WriteRegLocked(PCA9536_ADDRESS_7BIT, REG_OUTPUT, next & GPIO_MASK);
  if (!r) r = I2cBus_ReadRegLocked(PCA9536_ADDRESS_7BIT, REG_OUTPUT, &verify);
  if (!r && (verify & GPIO_MASK) != (next & GPIO_MASK)) r = I2C_BUS_TRANSFER_ERROR;
  I2cBus_Unlock(); return r;
}
