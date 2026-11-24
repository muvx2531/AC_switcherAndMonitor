# Agent guide

The STM32CubeIDE project is `F407_smartswitch/`. Preserve existing user changes;
check `git status` before editing. CubeMX-generated files contain `USER CODE`
sections: keep application changes in those sections or in separate files.

The agreed target architecture (v1) is in
[`docs/architecture/README.md`](docs/architecture/README.md): read it first, then only
the task/driver file you will change. Its invariants I1–I5 (relay state preserved
across reset, firmware never changes coil state on its own except the listed cases)
apply to every change. The architecture is not implemented yet; the bring-up docs
below describe current code behavior. Follow `docs/architecture/migration-plan.md`.

Before using or changing USART6 debug logging, read
[`docs/debug-logging.md`](docs/debug-logging.md). The document describes its API,
interrupt ownership, concurrency constraints, and current hardware test status.
UART, I2C and SPI HAL callbacks use per-handle registration
(`USE_HAL_*_REGISTER_CALLBACKS`, also set in the `.ioc`); never define global
`HAL_UART_*`, `HAL_I2C_*` or `HAL_SPI_*Callback` functions. Each driver registers
on its own handle in its init function.

The CubeMX boot-time `MX_ETH_Init` is bypassed. `eth_probe.c` powers and probes
the PHY using HAL, deinitializes HAL, then hands MAC/IRQ ownership to CycloneTCP
(MAC from STM32 UID, DHCP with static fallback). See `docs/ethernet-bringup.md` before
changing Ethernet initialization. The read-only status web (`web_server.c`, page in
`web/`, packed into `Core/Src/res.c` by `python tools/make_res.py`) is described in
`docs/web-status-api.md`; never edit `res.c` by hand.

Phase 1 foundation is in `docs/phase1-foundation.md`: only `supervisor.c` toggles the
STWD100 (PE7) after `main()`; long-running work in the default task must call
`Supervisor_Heartbeat()` at least every 2 s. The first I2C activity at boot is
`I2cBus_Recover()`; keep it before any PCA9536 or OLED access.

For the two SPI flash devices and the guarded destructive test, see
[`docs/flash-bringup.md`](docs/flash-bringup.md). Do not rerun the write/erase
test against nonblank or non-reserved storage. Use the `w25q128` DMA driver;
do not bypass its SPI2 mutex with the old blocking probe.

Before using I2C1, PCA9536, or relay outputs, read
[`docs/pca9536-bringup.md`](docs/pca9536-bringup.md). Physical relay operation
was confirmed by the owner on CH3 (2026-11-23); other channels not yet checked
individually. The user specifies LOW as coil OFF; do not infer actual
coil/contact state from I/O register readback. Since Phase 2 the Control task
(`control.c`, `docs/phase2-control.md`) is the only writer of the PCA9536 after
boot: change relays only through `Control_Submit`, never `PCA9536_WriteMasked`
from another module. Never send OFF or power-cycle commands to the board unless
the user has said energizing coils is safe at that moment. The web password is
shared by all boards and lives in the untracked `Core/Inc/web_secrets.h`
(template `web_secrets.example.h`): never commit it, print it or put it in docs.

The Acquisition service (`acquisition.c`, `docs/phase3-acquisition.md`) is the only owner
of ADC1/ADC2, TIM3 and DMA2 Stream0; read measurements with `Acquisition_GetSnapshot`.
Fit the DIS_WDT jumper before flashing images larger than 128 KB (sector erase > WDT timeout).

For the temporary USART6 command interface, read docs/serial-protocol.md. SET can actuate relays; checksum is not authentication.
