# Status LED and OLED (Local UI, display part)

2026-11-25. Firmware 0.7.0, bootloader 1.1.0. Owner request: make the board status readable
at a glance with the blue LED and the SSD1306 128x32 OLED (P402, I2C1 `0x3C`, no reset line,
so the panel keeps its picture across an MCU reset). Button relay control added in 0.8.0
(decisions S2-7 to S2-9), see the last section.

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

## Button control mode (0.8.0, 2026-11-25)

Owner decisions: D1 buttons have authority over the web **only while control mode is
active**; D2 the web may switch a relay OFF and leave it off; D3 the UX below.

| Action | Effect |
|---|---|
| Short press SW1 or SW2 | wake the display / next status page (unchanged) |
| Hold SW1 or SW2 for 5 s | "Keep holding 5 s" bar, then CONTROL MODE (only if the OLED works) |
| SW1 press | select the next item: CH1 -> CH2 -> CH3 -> CH4 -> EXIT -> CH1 (selected channel inverted) |
| Hold SW2 for 2 s | toggle the selected channel ON <-> OFF (`Control_Submit`, `SRC_LOCAL`), or leave on EXIT; progress bar, one action per press |
| No press for 10 s | back to the status pages (countdown top right) |

The buttons that entered the mode must be released before they count. Results come from
Control ("CH2 OFF: done", "Refused: no 24V", ...); OFF is refused without the 24 V coil
supply (rule (c)). There is no power-cycle from the buttons. While the mode is active
Control refuses web and serial commands (`LOCAL_CONTROL`, HTTP 409) and the web page shows a
red note; the lock expires by itself 15 s after the UI stops refreshing it. Button commands
are in the event log as "from local".

Bench test (board 2, 0.8.0 through FOTA): enter by holding 5 s, selection loops with EXIT,
CH2 OFF then ON from SW2 (load followed), timeout back to the status pages after 10-11 s, EXIT
leaves at once, web `SET_ON` during the mode -> 409 `LOCAL_CONTROL`, after leaving -> 200.
