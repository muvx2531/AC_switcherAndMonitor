# Phase 1: foundation (implemented 2026-11-23)

Branch `feature/phase1-foundation`. Implements the parts of architecture Phase 1 that the
Control service needs. See `docs/architecture/migration-plan.md`.

| Item | Where | Behavior |
|---|---|---|
| Reset cause | `boot.c` `Boot_CaptureResetCause` (first line of `main`) | Reads and clears `RCC_CSR`. Reported in the boot log and `/api/status` (`sys.reset_cause`). An STWD100 reset drives NRST, so it appears as `PIN` |
| RTC kept across reset | `main.c` `MX_RTC_Init` USER CODE | `BKP0R` = `0x52544331` ("RTC1") means the calendar is valid: the generated set-time code is skipped. Written once after the first set. `sys.rtc_kept` |
| Watchdog boot phase | `boot.c` `Boot_WdtEarlyKick` (USER CODE SysInit), `Boot_WdtKick` (USER CODE 2) | Toggles PE7 (Clr_wdt) before `MX_GPIO_Init` and after peripheral init |
| Minimal Supervisor | `supervisor.c` | Static task, stack (1 KB) in CCM, priority BelowNormal. Kicks PE7 every 200 ms. Boot phase until `Supervisor_BootComplete()` or 15 s deadline; then only while the default-task heartbeat is < 2 s old. Logs once when it stops kicking. `sys.watchdog.phase`: `boot`, `run`, `boot_timeout`, `heartbeat_lost`. No other module toggles PE7 after `main` |
| Unused interfaces safe | `boot.c` `Boot_SafeUnusedInterfaces` (USER CODE 2) | De-initializes USART1 (EC25) and USART2 (RS485) so PA9/PD5 stop driving into unpowered rails; PD1, PE1, PC9 low |
| I2C recovery | `i2c_bus.c` `I2cBus_Recover` | 9 SCL clocks + STOP by GPIO, I2C1 software reset, DeInit/Init (DMA, IRQ), callbacks re-registered. Writes no slave register. Runs once at boot before the first PCA9536 read (S6-1) and automatically from `I2cBus_Lock` (max 1/s) after a transfer timeout. `sys.i2c_recoveries` |
| Web | `web/index.html`, `res.c` | Relays tab shows "Relay control is not available on this firmware" when `control.available` is absent; System card shows reset cause, watchdog phase, RTC and I2C recoveries |

**Deferred:** the 32 KB bootloader reservation (app at `0x08008000`). Without a bootloader
the MCU would boot into empty flash, so the linker change moves to Phase 5 together with the
bootloader itself. Decision S7-3 (bootloader in v1) is unchanged.

## Hardware test (2026-11-23, Debug build, DIS_WDT jumper fitted, relays not fitted, no AC)

1. Flash + reset: `BOOT: reset cause POWER_ON, RTC set to default` (first boot of this
   firmware), PCA9536 `config=0 latch=0` before and after, flash IDs, DHCP fallback, web.
2. Hard reset via ST-LINK: `BOOT: reset cause PIN, RTC kept`; `/api/status`:
   `watchdog=run`, `rtc_kept=true`, `i2c_recoveries=1`, PCA read OK with outputs unchanged.
   Minimum stack free (words): IDLE 105, Tmr Svc 214, debugTx 217, supervisor 226.
   Heap minimum 22,320 B.

## Running without the DIS_WDT jumper (2026-11-25, firmware 0.7.1, board 2)

Before removing the jumper the remaining unbounded waits were fixed: the CycloneTCP
STM32F4 MAC driver waited forever for the DMA software reset (needs the PHY's 50 MHz RMII
clock; a dead or unpowered PHY would starve the Supervisor and cause a reset loop) and for
MDIO busy. These waits are now bounded (100 ms, returns `ERROR_TIMEOUT`, the interface start
fails and is retried; MDIO ~ms). The change is marked `SmartSwitch:` in
`Middlewares/.../drivers/mac/stm32f4xx_eth_driver.c` (GPLv2: publish with the source).

`Boot_WdtKick` measures the longest gap between kicks (DWT cycle counter). The log prints
`SUP: boot phase done in .. ms, longest watchdog gap .. ms (limit 1120)` and `/api/status`
shows `sys.watchdog.boot_gap_ms` and `max_gap_ms` (run phase). Serial `WDTTEST` stops the
default task heartbeat to prove the reset path.

| Test (jumper removed) | Result |
|---|---|
| Boot gap / run gap | 200-201 ms / 200 ms (limit 1120 ms); boot phase 1.0 s |
| Power on without jumper | boots, `POWER_ON`, keeps running |
| Serial `WDTTEST` | Supervisor stops kicking 2 s later (`heartbeat_lost: defaultTask`), bootloader restarts 1.6 s after that, `reset_cause PIN`, PCA9536 latch unchanged |
| FOTA install 0.7.0 -> 0.7.1 (sector erase in the bootloader) | no watchdog reset; installed, trial, committed after 60 s |
| Upload 240 KB over the web | run gap still 200 ms |

The ST-LINK still needs the jumper (erasing a 128 KB sector takes longer than 1.12 s and
the debugger halts the CPU). Fit it before flashing, remove it afterwards.

## Not yet tested (needs someone at the board)
- **I2C recovery under a real fault:** briefly short SDA (PB7) to GND while the status page
  polls; expect `relay.ok=false`, then recovery and `i2c_recoveries` increasing, with the
  PCA9536 latch unchanged.
- **Relay latch preserved across reset with recovery:** set a pattern with serial `SET`,
  reset, check the boot log shows the same latch before and after (only when it is safe to
  energize coils).
