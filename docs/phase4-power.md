# Phase 4: Power service, Supervisor health registry, safe mode

Branch `feature/phase4-power`, 2026-11-25. Implements
[tasks/power.md](architecture/tasks/power.md) and the Phase 4 part of
[tasks/supervisor.md](architecture/tasks/supervisor.md).

Owner decisions for this phase (2026-11-25):
- LOW_POWER does **not** reduce the clock (S6-3 clock reduction deferred).
- Turn ETH off only after 24 V has been absent for **2 s**; back on after 24 V is present for **1 s**.
- LOW_POWER when VBAT < 6.2 V (held 10 s) **or** 8 h on battery.
- Rule (c) stays: after a 24 V outage every channel stays ON (an earlier SET_OFF is not restored).

## Power (`power.c`)

Task `power`, static, 384-word CCM stack, `osPriorityNormal1`, polls every 100 ms.

| Mode | Entered when | Rails |
|---|---|---|
| MAINS | boot, or 24 V present ≥ 1 s | ETH on (resume retried every 5 s if the PHY does not answer), AMC on |
| BACKUP | 24 V absent ≥ 2 s | ETH off (`EthProbe_Suspend`), AMC off |
| LOW_POWER | in BACKUP: VBAT < 6200 mV for 10 s, or 8 h | same as BACKUP; clock unchanged |

- The 24 V fact is Control's confirmed `coil_supply` (PB3 EXTI + confirm, S4-2). Power never
  touches relays; Control already releases latches on 24 V loss (rule (c)).
- The +3.3V_AMC rail (PC10) moved here from Acquisition. With `ACQ_AMC_FITTED 0` it stays off.
- In safe mode the network is never started, so Power never turns ETH on.

### ETH suspend / resume (`eth_probe.c`)

- Suspend: stop DHCP → `netStopInterface` (link down, IRQ off, no PHY polling) → stop MAC/DMA
  → RMII + MDC/MDIO pins back to analog (no back-feed into the unpowered PHY) → MAC clocks off
  → PE0 low.
- Resume: PE0 high → 100 ms → pins to AF11 → bounded MDIO read of the PHY ID (10 × 50 ms; the
  CycloneTCP LAN8742 driver would busy-wait forever on a dead PHY) → `nicDriver->init` under
  the stack mutex (MAC reset, descriptors, PHY init) → `netStartInterface` → DHCP again unless
  the static fallback was in use (then the same static address is kept).

## Supervisor (`supervisor.c`)

- Heartbeat registry: `defaultTask`, `control`, `acq`, `power` (`Supervisor_TaskHeartbeat`).
  A task is checked after its first heartbeat; any heartbeat older than 2 s stops the PE7 kick
  and the log names the task (`SUP: stopped kicking watchdog (heartbeat_lost: <task>)`).
- After 10 min in run phase the boot-loop counter is cleared.

## Safe mode (`boot.c`, decision S6-5)

- `Boot_EvaluateSafeMode()` runs first in the default task. BKP5R = `0x5243` | reset count,
  BKP6R = window start (RTC seconds since 2000). POWER_ON resets the count to 0 (not a loop).
  With a kept RTC the window is 10 min; without it the count is consecutive and only cleared by
  10 min of stable run or by the serial command.
- Count ≥ 5 → **safe mode**: Control, Power and Supervisor only (no Acquisition, flash, network).
  Log: `BOOT: ... reset count 5 -> SAFE MODE ...`.
- Clear: serial `CLRSAFE` (`tools/relay-command.ps1 -Command CLRSAFE`), effective at the next reset.
  Note for development: five quick reflashes/resets within 10 min also trigger safe mode.

## Status API additions

`power`: `mode`, `mode_age_s`, `eth_rail`, `amc_rail`, `transitions`, `eth_resume_fail`, `reason`.
`net.powered`. `sys`: `reset_count`, `safe_mode`, `watchdog.lost`, `watchdog.hb_ms{task: ms|null}`.
The page shows Supply mode, ETH/AMC rail, Safe mode and Reset count.

## Hardware test 2026-11-25

Board 2 (MAC 02:1D:51:08:57:A2), AMC3330 fitted, DIS_WDT jumper fitted, Debug build,
0 errors / 0 warnings.

| Test | Result |
|---|---|
| Boot | MAINS, ETH and AMC rails on, all four heartbeats fresh; PCA latch kept |
| Safe mode | SWD resets 1–4 counted 1→4, 5th boot `SAFE MODE` (no ACQ/flash/ETH); `CLRSAFE,OK` then reset → count 1, normal boot. Latch 3 kept through all 7 resets (I1) |
| 24 V removed, 3 cycles | rule (c) released latches immediately; `ETH: suspended` + `MAINS -> BACKUP` after 2.04–2.06 s |
| 24 V restored, 3 cycles | `BACKUP -> MAINS` after 1.01–1.09 s, `ETH: resumed` 0.2 s later; page reachable at the same IP; 0 resume failures |
| Heap | minimum ever 17.5 KB after the first cycle, unchanged after two more (no leak seen) |
| Contact feedback (owner's own SET_OFF via web) | CH1/CH2 OFF → 3.1 V, other lines 229.8 V |
| Stack free (words) | power 186, acq 290, control 296, supervisor 226 |

Not tested: WDT reset on a lost heartbeat with the DIS_WDT jumper removed, LOW_POWER entry
(needs a temporary higher threshold or a discharged battery), back-feed measurement on the
unpowered PHY rail, boot without 24 V (on battery), many more 24 V cycles / long run.
