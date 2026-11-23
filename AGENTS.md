# Agent guide

The STM32CubeIDE project is `F407_smartswitch/`. Preserve existing user changes;
check `git status` before editing. CubeMX-generated files contain `USER CODE`
sections: keep application changes in those sections or in separate files.

Before using or changing USART6 debug logging, read
[`docs/debug-logging.md`](docs/debug-logging.md). The document describes its API,
interrupt ownership, concurrency constraints, and current hardware test status.
UART, I2C and SPI HAL callbacks use per-handle registration
(`USE_HAL_*_REGISTER_CALLBACKS`, also set in the `.ioc`); never define global
`HAL_UART_*`, `HAL_I2C_*` or `HAL_SPI_*Callback` functions. Each driver registers
on its own handle in its init function.

The CubeMX boot-time `MX_ETH_Init` is bypassed. `eth_probe.c` powers and probes
the PHY using HAL, deinitializes HAL, then hands MAC/IRQ ownership to CycloneTCP
for a temporary static-IP ping test. See `docs/ethernet-bringup.md` before
changing Ethernet initialization.

For the two SPI flash devices and the guarded destructive test, see
[`docs/flash-bringup.md`](docs/flash-bringup.md). Do not rerun the write/erase
test against nonblank or non-reserved storage. Use the `w25q128` DMA driver;
do not bypass its SPI2 mutex with the old blocking probe.

Before using I2C1, PCA9536, or relay outputs, read
[`docs/pca9536-bringup.md`](docs/pca9536-bringup.md). Physical relay operation
remain unconfirmed. The user specifies LOW as coil OFF; do not infer actual
coil/contact state from I/O register readback.

For the temporary USART6 command interface, read docs/serial-protocol.md. SET can actuate relays; checksum is not authentication.
