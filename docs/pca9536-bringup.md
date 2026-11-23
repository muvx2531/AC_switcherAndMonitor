# PCA9536 and relay bring-up

The PCA9536DGKR is on I2C1, 7-bit address `0x41`, PB6 SCL / PB7 SDA at
400 kHz. DMA1 Stream 0 is I2C1 RX and Stream 7 is TX. `i2c_bus.c` uses
`HAL_I2C_Mem_Read_DMA` / `HAL_I2C_Mem_Write_DMA`; DMA completion/error wakes a
task via a binary semaphore. A static mutex serializes the whole I2C1 bus;
the PCA driver holds it over read-modify-write. OLED and future clients must
use the same bus manager (its current public helper only transfers one-byte
registers; extend it for OLED data). API calls are task-context only and wait
with timeouts. On transfer timeout, the driver stops DMA and latches the bus
faulted until reset; the I2C peripheral/device state may be uncertain.

`Core/Inc/pca9536.h` and `Core/Src/pca9536.c` implement register reads,
startup output configuration and masked read-modify-write of the output latch,
with output/config register read-back. The user requires that all four pins be
outputs, with LOW meaning relay coil OFF. `PCA9536_InitOutputsKeepState` reads
the existing configuration and output latch; pins already outputs retain their
latch state. Pins currently inputs have their output latch preloaded LOW before
their direction is changed to output. The operation is mutex-protected and
read-back verified. If it fails after a write, physical state is uncertain and
must not be represented as confirmed. Initialization runs once on each MCU
startup; it must not assume the expander was reset with the MCU.

`PCA9536_Configure` writes
the output latch before changing directions; **it can still change already
configured outputs immediately**. `PCA9536_WriteMasked` changes hardware.
`PCA9536_ReadRegisters` itself is read-only.
Call `I2cBus_Init(&hi2c1)` once after the scheduler starts. Check return codes;
after a timeout or error, do not infer relay state from the requested value.
No application relay-to-output mapping, AC-contact meaning or fault policy is
defined here. Do not treat a LOW coil command as necessarily AC load OFF: the
contact wiring must be verified.

HAL I2C DMA writes require the I2C event and error IRQ handlers in addition
to DMA IRQs. They are enabled at priority 5 in the `I2C1_MspInit 1` USER CODE
section; handlers are in `stm32f4xx_it.c` USER CODE. CubeMX `.ioc` currently
does not mark I2C1 EV/ER IRQs enabled. If enabling them in CubeMX later, remove
the manual handlers/enable calls first to prevent duplicate symbols, and retain
priority 5 or another FreeRTOS-compatible priority. `USE_HAL_I2C_REGISTER_CALLBACKS`
is enabled: `I2cBus_Init` registers static Mem-RX/Mem-TX complete and error
callbacks on the I2C1 handle; no module defines the global `HAL_I2C_*Callback`
functions. Other bus clients (OLED) go through `i2c_bus.c` and must not
re-register callbacks on I2C1. Built 2026-11-23; not yet re-tested on hardware.

On 2026-11-19, read-only DMA test on the board returned input `F0`, output
`FF`, polarity `00`, configuration `FF`: lower four pins were inputs. With the
user confirming no AC or load connected, a one-shot test changed IO0..IO3
high one at a time, read input/output bits back, then restored original output
and configuration. All register and pin read-backs and restoration passed.
This confirms PCA pin level/readback, **not physical relay coil/contact
operation**. The user did not hear or observe relay motion, despite reporting
24 V supplied. Measure +24Vcoil relative to CoilGND on the assembled revision
and check optocoupler, driver, coil voltage and contacts before concluding.
The PDF in the project was exported 2026-10-22; newer `.SchDoc` files exist,
so verify against the actual PCB/schematic revision when interpreting wiring.

An earlier debugger-triggered test changed all four outputs and restored the
initial input configuration. It has since been removed because this board uses
the pins as outputs. A relay test needs physical observation or coil/contact
measurements, not just register read-back. PCA9536 can retain its state across
MCU reset. Do not switch outputs without knowing board wiring and load state.

Debug and Release builds passed. Multi-task bus contention, timeout recovery,
OLED coexistence, and physical relay operation remain to be verified.
