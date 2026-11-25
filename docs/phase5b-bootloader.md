# Phase 5b: Bootloader and firmware update (FOTA)

2026-11-25. Owner decisions: separate CubeIDE project (`Bootloader/`, created by the owner,
cleaned up to the minimum), **SHA-256 integrity only in v1** (no signature; a 64-byte field is
reserved in the header), safe and simple for field staff.

## Flash map

| Where | What |
|---|---|
| internal `0x08000000` (sectors 0-1, 32 KiB) | bootloader (13 KiB used) |
| internal `0x08008000` | application: 512-byte header, vectors at `0x08008200`, up to 992 KiB |
| chip 2 `0x000000` (1 MiB) | staging slot (uploaded image) |
| chip 2 `0x100000` (1 MiB) | backup slot (last known-good image, rollback) |
| chip 2 `0x200000` / `0x201000` | boot record A/B (sequence + CRC-32, highest valid wins) |

Format: `ssw_image.h` (identical copy in both projects). Header: magic (`SSWF` release / `SSWD`
debug), board id `SSW-F407`, version, build string, load address, vector offset, image end,
payload size, SHA-256 of the payload, reserved signature field, header CRC-32.

## Bootloader (`Bootloader/`, build **Release** (-Os))

- CubeMX config reduced to RCC (HSE 16 MHz, PLL 168 MHz), SYS (SWD, SysTick), SPI2, USART6 and
  four GPIOs (PE7 watchdog, PD7/PD13 flash CS, PC11 LED). No I2C, RTC, ADC, DMA: **relay state,
  RTC and backup domain are never touched**; RCC reset flags are kept for the application
  (clock reset done by hand, not `HAL_RCC_DeInit`).
- STWD100 fed from the first instruction; internal sector erase runs from RAM with interrupts
  off and toggles PE7 every ~100 ms (128 KiB sector erase takes 1-2 s).
- Boot logic (`bl_main.c`): read boot record, then
  - `PENDING`: verify staging (header + SHA-256) → copy the running image to the backup slot
    and verify it → `INSTALLING` → erase + program internal flash from staging → verify →
    `TRIAL`.
  - `INSTALLING` after a power cut: staging still valid → program again.
  - `TRIAL`: count the boot; after 3 boots without confirmation → `ROLLBACK`.
  - `ROLLBACK`: verify backup hash → program it → `IDLE` (result `ROLLED_BACK` or `RESTORED`).
  - Always: check the internal image (release: SHA-256; debug build: header + vector table
    plausibility). Corrupt → restore backup, else install a valid staging image, else
    **rescue mode** (LED blinks, watchdog fed, program with ST-LINK).
- Logs on USART6 (`BL: ...`). Normal boot adds ~30 ms (release image: + SHA-256 of ~220 KiB).

## Application side

- Linker: FLASH at `0x08008000` (992 KiB), `.ssw_header` first, `.isr_vector` 512-B aligned,
  `_ssw_image_end`; `main()` sets `SCB->VTOR`. `Core/Src/app_image.c` holds the header (DEV);
  version in `Core/Inc/fw_version.h`.
- `tools/make_ssw.py` turns the build into a release image: `build/smartswitch-<ver>.ssw`.
- `fota.c` (chip 2): streaming upload into staging (sectors erased as data arrives), header +
  SHA-256 verify by reading back, install / rollback request (boot record) then reboot 1.5 s
  later, **confirms a trial image after 60 s in Supervisor run phase**. Events `ST_EV_FIRMWARE`.
  In safe mode FOTA is not started, so a trial image that boot-loops is rolled back.
- Web: `GET /api/fw/status`, `POST /api/fw/upload?token=<session>` (raw body),
  `POST /api/fw/apply` and `/api/fw/rollback` `{token, password}` (password re-check, lockout
  applies). Firmware tab: upload `.ssw` with progress, Install, Roll back, steps, history.
- Static page now sent with `Cache-Control: no-cache` so a browser reloads the page that
  matches the new firmware.

## Release procedure

1. Bump `Core/Inc/fw_version.h`, build `F407_smartswitch` (Debug or Release).
2. `python tools/make_ssw.py` → `build/smartswitch-<ver>.ssw`.
3. Web → Firmware → choose the file → Upload → Install and reboot (password).
4. The page reconnects after ~25 s; the new firmware shows "trial 1/3" and becomes
   "confirmed" after 60 s.

Factory / bench: flash `Bootloader/Release/Bootloader.elf` once, then the application ELF
(ST-LINK writes only the sectors it needs). Never use a full-chip erase without re-flashing the
bootloader. Fit the DIS_WDT jumper when flashing with ST-LINK (application sector erase >
1.12 s). Without the bootloader the application does not start.

## Bench test 2026-11-25 (board 2)

| Test | Result |
|---|---|
| Builds | bootloader 13.2 KiB, 0 errors (1 linker note: RWX segment from the RAM function); application 0 errors / 0 warnings |
| Boot via bootloader | debug image accepted, application starts ~30 ms after reset; reset cause preserved |
| Upload 224 KiB `.ssw` | 4.7-4.8 s, verified VALID; random file rejected ("not a SmartSwitch image"); wrong password on install → 403 |
| Install 0.6.0 → 0.6.1 | backup 5.4 s + program 3.6 s, trial 1/3, **committed after 60 s**; relay latch unchanged |
| Auto-rollback | 0.6.2 built to never confirm: trial 1/3, 2/3, 3/3 (manual resets), 4th boot rolled back to 0.6.1 in 4.3 s, result `ROLLED_BACK` |
| User rollback | `RESTORED` in 3.5 s |
| Install through FOTA of the no-cache fix | trial → committed |
| Power cut during backup (24 V pulled, battery removed) | cut 0.2 s into the backup; on power-up state still PENDING → backup redone (5.5 s) → program → trial → committed |
| Power cut during programming | cut while writing internal flash; on power-up state INSTALLING → program again from staging (3.7 s), SHA-256 OK → trial → committed |
| Power cut during user rollback | cut while writing from the backup slot; on power-up state ROLLBACK → program again (3.7 s) → `RESTORED` |

Found and fixed during the test: I2C1 stuck in master mode with SCL held low after a failed
transfer start; `i2c_bus.c` now marks the bus faulted on a start error so the next access runs
the bus recovery (verified by forcing the fault over SWD: recovered within 1.5 s).

Power cuts were made by hand on a spoken cue from a serial-log watcher; the battery must be
unplugged first, otherwise the board just switches to battery and the MCU keeps running.

Not tested yet: watchdog
reset during an install with the DIS_WDT jumper removed, rescue mode, upload interrupted
(cable pulled), images near the 992 KiB limit.
