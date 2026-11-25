# Phase 5a: Storage service

Branch `feature/phase5a-storage`, 2026-11-25. Implements
[tasks/storage.md](architecture/tasks/storage.md) for chip 1; chip 2 is left for the
bootloader/firmware slots (Phase 5b). Owner decisions: D8 partition as proposed, split
Phase 5 into 5a (storage) and 5b (bootloader + FOTA, separate CubeIDE project).

## Pre-check

A read-only whole-chip scan (`FLSCAN`, serial) found **both chips completely blank**
(0 of 256 x 64 KiB blocks non-blank each, ~36 s per chip at SPI 5.25 MHz).

## Layout (chip 1)

| Address | Size | Content |
|---|---|---|
| 0x000000 / 0x001000 | 4 KiB each | config A / B (highest valid sequence wins, write goes to the other slot) |
| 0x002000 | 4 KiB | superblock `SSST`, layout 1 |
| 0x010000 | 64 KiB | energy checkpoints (32 B records) |
| 0x020000 | 1 MiB | event log (32 B records, ~32k events) |
| 0x120000 | 4 MiB | PQ history, 32 B minute records (~91 days) |
| 0xFFF000 | 4 KiB | kept for the bring-up flash test |

Every record: sequence number first, CRC-32 last. Rings erase a 4 KiB sector just before it
is reused (oldest data dropped). Programs are read back. Mount reads the first record of every
sector, then scans the newest sector (~120 ms at boot for 1300 sectors).

First boot: superblock absent and every partition sector starts blank -> superblock written.
If foreign data is found the state is `UNFORMATTED` and nothing is written until serial
`STFORMAT` (erases the partitions, ~1 min). Relay state is never restored from flash (I4).

## Service (`storage.c`)

- Task `storage`, `osPriorityLow` (below Supervisor), 512-word CCM stack, queue of 24 requests
  (non-blocking producers; full queue -> `dropped`). Heartbeat `SUP_TASK_STORAGE`.
- `Storage_Init()` runs in the default task before Acquisition: mount, config, energy restore.
- Events: boot (reset cause, reset count), relay command outcomes, rule (c) releases, 24 V
  lost/present, supply mode changes, PQ sag/swell/interruption (at the end, with duration and
  extreme), contact faults, CT reversed, config changes.
- PQ history: each 1-minute entry from `pq_history.c` is also written to flash with the RTC
  time.
- Energy: checkpoint every 10 min and when Power leaves MAINS; restored into the PQ counter at
  boot.
- Board config (per board, survives reflashing): `amc_fitted` (0/1, 0xFF = build default) and
  CT phase (0.01 degree). Serial `AMC,0|1|D` and `CTPH,<n>` (effective after reset);
  `tools/relay-command.ps1 -Command RAW -Raw 'AMC,1'`.

## Web

- `/api/status` `storage{state, event_seq, history_records, energy_saved_mwh, dropped,
  write_errors, config{seq, amc, ct_phase_cdeg}, amc_active, rtc_s, rtc_kept}`.
- `GET /api/events?n=N` (max 40, newest first): `{rtc_s, uptime_s, events[{seq, rtc_s, up_s,
  type, a, b, c}]}`.
- `GET /api/history?range=24h|7d|30d|90d`: binary `PQH2` (16-byte header with bucket size and
  RTC now, then `u32 rtc + 9 x i16` entries), averaged into 1/10/30/60-minute buckets, chunked
  transfer; falls back to the RAM ring when flash history is unavailable.
- Page: Quality tab ranges 24 h / 7 d / 30 d from flash; Status tab Event log table; System
  card shows storage state, saved energy and the board config.

Timestamps use the board RTC (seconds since 2000). The RTC is not set from a real clock yet;
the page maps RTC to wall time relative to "now", so history survives resets as long as the RTC
keeps running (coin cell). Setting the clock comes with the network settings (Phase 6).

## Bench test 2026-11-25 (board 2)

| Test | Result |
|---|---|
| Build | 0 errors / 0 warnings; CCM 44.8 KB / 64 KB, SRAM ~118 KB / 128 KB |
| First boot | superblock written, `STORAGE: ready`, mount 116-128 ms on later boots |
| Events | boot events logged with reset cause and count; config changes logged |
| History | 1 record per minute, readable with `range=24h` and `7d` |
| Config A/B | `AMC,1` and `CTPH,0` queued -> config seq 2 -> survives reset |
| Status / stacks | storage 414 words free, HTTP 287, acq 331, power 266; heap minimum unchanged; 0 write errors, 0 dropped |

Not tested yet: energy restore after reset (needs a checkpoint: 10 min uptime or a 24 V
removal), power cut during a config / log write (Phase 5 gate), ring wrap-around, UNFORMATTED
path with foreign data, 30-day history read time.

Observed during testing (not caused by storage): after a board reset the PC on the direct
cable answers ping only ~65 s later although the board logs its static address at 12.8 s;
with the PHY link up on both sides from ~5 s. Still under investigation. Frequent test resets
also reached the safe-mode threshold once (no Ethernet in safe mode); clear with `CLRSAFE`.
