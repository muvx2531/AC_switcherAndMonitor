# Status LED and OLED (Local UI, display part)

2026-11-25. Firmware 0.7.0, bootloader 1.1.0. Owner request: make the board status readable
at a glance with the blue LED and the SSD1306 128x32 OLED (P402, I2C1 `0x3C`, no reset line,
so the panel keeps its picture across an MCU reset). Relay control from the buttons (open
item D3) is not part of this step.

## LED (PC11, one LED, active high)

Patterns repeat every 2 s (100 ms slots); the most important state wins.

| State | Pattern | Who |
|---|---|---|
| Bootloader flash work (install, rollback, repair) | flicker 100 ms on / 100 ms off (still during a sector erase, 1-2 s) | bootloader |
| Bootloader rescue (no valid firmware) | 1 s on / 1 s off | bootloader |
| Firmware update requested, rebooting | flicker | app |
| Fault: safe mode, relay state unknown, ADC stopped | 100 ms on / 300 ms off | app |
| LOW_POWER | 1 flash every 5 s | app |
| Warning: battery (no 24 V), no LAN cable, storage problem, CT backwards, supply event | 2 flashes | app |
| Booting (Supervisor boot phase) | steady on | app (and bootloader during a normal boot) |
| Firmware trial (not confirmed yet) | 3 flashes | app |
| Normal | 1 flash | app |

## OLED pages (application, `Core/Src/local_ui.c`)

Pages rotate every 4 s and refresh every 1 s: main page, one page per active alert, info page.

- Main: status tag (inverted `OK`, `WARN`, `FAULT`, `SAFE`, `TRIAL`, `LOW`, `BOOT`) + supply
  mode + firmware version / `1:ON 2:ON 3:OFF 4:ON` (effective state, `CYC` during a power
  cycle, `?` if unknown) / voltage, current, frequency / IP (`S` = static fallback) or the LAN
  state.
- Alert pages: inverted title, two lines of detail, relay line.
- Info: MAC / W, PF, THD / energy (kWh) / uptime.
- Firmware update requested: "FIRMWARE UPDATE / Installing x.y.z / Rebooting... /
  DO NOT POWER OFF" until the reset.
- Brightness: dims after 5 min without a button press, back to full on any warning or fault.
  SW1/SW2 (polled, active low): first press wakes the display, next presses step pages.
  No relay command from the buttons.

Rules kept: no OLED traffic before Control has done the first PCA9536 read (S6-1); one I2C
transaction per page (control byte + 128 bytes, ~3 ms at 400 kHz) and the bus lock is released
between transactions (`I2cBus_WriteBlockLocked`); an absent OLED is retried every 10 s and
never affects the relays. The UI task is not supervised, so a stuck display never resets the
board.

## Bootloader screens (`Bootloader/Core/Src/bl_main.c`)

The owner added I2C1 to `Bootloader.ioc` (100 kHz). The bootloader writes only to `0x3C`,
only while it has flash work to do (a normal boot sends nothing on I2C), never runs bus
recovery clocks, and skips the display for the rest of the boot on any I2C error. I2C1 and
DMA1 are de-initialised and reset before the jump to the application.

Screens: FIRMWARE UPDATE / Installing x.y.z / DO NOT POWER OFF / progress bar with the step
(Verify, Backup, Check, Erase, Write, Check), then "Installed x.y.z / Starting, trial run".
Also "Resuming install" (after a power cut), "UPDATE FAILED / Restoring previous",
"RESTORING FIRMWARE" (user rollback or trial not confirmed), "REPAIRING FIRMWARE",
"NO VALID FIRMWARE / Program with ST-LINK".

The shared renderer `ssw_oled.c/.h` (5x7 font, SSD1306 init, text, double size text,
progress bar) is an identical copy in both projects, like `ssw_image.h`.

## Bench test 2026-11-25 (board 2)

| Test | Result |
|---|---|
| Builds | bootloader 18.0 KiB of 32 KiB, 0 errors; application 0 errors / 0 warnings |
| App pages, LED normal pattern, button page step | as designed (owner checked) |
| Install 0.6.1 DEV -> 0.7.0 through the web | app screen before reboot, bootloader progress screens, LED flicker, trial page and 3 flashes, committed after 60 s (owner checked) |
| Install time with display updates | backup 6.0 s (was 5.4), program 4.8 s (was 3.6) |

Not tested yet: OLED unplugged at boot and while running (retry, relays unaffected), rescue
screen, LOW_POWER pattern, burn-in over months.
