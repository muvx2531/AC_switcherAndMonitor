# Backup test command

`@SS,BACKUP*CC\r\n` uses the existing additive checksum framing described
in `serial-protocol.md`. Response body is `SS,BACKUP,ADC,CHRG,FAULT,VIN,EFUSE`.
ADC is decimal ADC2 channel 13 rank 9, 0..4095; the remaining fields are raw
GPIO levels (0 or 1): PE13 Batt_status, PE11 Batt_FAULT, PB3 Vin_Detect,
PB9 eFuse_iLim. These are samples, not proof of charger, input-power, battery
or fault state. Check actual circuit polarity, reference, divider and calibrate
before converting to volts or using in protection decisions.

`@SS,BATV*CC\r\n` returns `SS,BATV,ADC,ESTIMATED_MV,VREF_MV`. It uses
the nominal 3000 mV reference and divider 56k/(49.9k+49.9k+56k), rounded to
the nearest mV: `ADC * 3000 * 1558 / (4095 * 560)`. This is an estimate,
not a calibrated measurement; VREF+ and component tolerances are unverified.
The GPIO/ADC caveats below also apply. Saturation and stale DMA samples must
be handled before use in protection or charging decisions. From PowerShell:
`.\tools\relay-command.ps1 -Command BATV`.

The first request starts ADC2 continuous 10-rank circular DMA in a dedicated
10-halfword static buffer and waits for rank 9. Later requests read the latest
value. If the ADC is already running under another owner the command rejects
the request; if startup or HAL reports an error it returns `SS,ERR,ADC`.
Integrate with a unified ADC acquisition module before another consumer starts
ADC2 DMA; do not let two modules own ADC2. No freshness guarantee is currently
provided after the first sample if DMA silently stops, and GPIO/ADC readings
are not a simultaneous snapshot. Do not use these raw readings for control.

In PowerShell: `.\tools\relay-command.ps1 -Command BACKUP`. The command is
read-only with respect to relay and power-control outputs. ADC DMA is started
by the first request. On 2026-11-21 the prototype returned
`@41,BACKUP,3207,1,1,0,0*B1`, followed by ADC 3205; user reported battery
voltage 6.55 V at approximately the same stage. UART RX can stop after an error and require
MCU reset; this pre-existing issue remains unresolved.
