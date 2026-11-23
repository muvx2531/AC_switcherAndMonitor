# Phase 2: Control service (implemented 2026-11-23)

Branch `feature/phase2-control`. Design: `docs/architecture/tasks/control.md`.

## What changed

| Part | Where | Notes |
|---|---|---|
| Control task | `Core/Src/control.c` | Static task, 2 KB stack in CCM, priority AboveNormal. Only writer of the PCA9536 after boot. Queue of 8 requests, outcome history of 16, snapshot published every loop (20 ms) |
| Commands | `Control_Submit` | `SET_ON`, `SET_OFF`, `POWER_CYCLE` (1–600 s, default 10), `CANCEL_CYCLE`; `channel_mask`; `source` WEB/SERIAL/LOCAL/SYSTEM. A new command on a cycling channel supersedes it (S5-3). `SET_OFF`/`POWER_CYCLE` rejected without coil supply (S5-5); every command rejected while the expander state is unknown (I3) |
| Rule (c) | `evaluateSupply`, `releaseAll` | PB3 EXTI3 (both edges) wakes the task; loss confirmed by 3 reads 1 ms apart; then all latches LOW, pending cycles cleared, write retried until it succeeds. Return confirmed after 200 ms stable. Polling every 20 ms as backup and at boot. Rejected edges counted as `glitches` |
| PB3 polarity | `VIN_PRESENT_LEVEL` | HIGH = 24 V present, from the bench reading with 24 V connected. **Confirm by removing 24 V (open item H2)**; if wrong, rule (c) releases latches and OFF is rejected (safe direction, but unusable) |
| pending_restore | RTC BKP DR1–DR4 | DR1 = `0x5052xxxx` + channel mask (written last, cleared first), DR2/DR3 = durations per channel, DR4 = check value. Boot: resume a cycle only where the latch is still HIGH, for the full duration from boot; otherwise drop (I4) |
| Supervisor | `supervisor.c` | Run phase now also needs the Control heartbeat (< 2 s) |
| Serial | `debug_protocol.c` | `SET,x` = one `SET_OFF` for bits set and one `SET_ON` for the rest, through Control; waits up to 1 s for each outcome. New errors `ERR,<reason>` |
| Debug hooks | `main.c` | `relayPatternRequest` / `relay3TestRequest` removed (they wrote the PCA directly) |
| Web | `web_server.c`, `web_auth.c`, `web/index.html` | Sign-in, relay commands, outcomes, `control` in `/api/status`. See `docs/web-status-api.md` |

## Web sign-in (bench version)

- Password is derived from the STM32 UID (unique per board) and printed on the serial
  console at boot: `WEB: bench password ss-xxxxxxxx`. A stored, changeable password comes
  with the Storage service.
- `POST /api/login` returns a 128-bit random session token (STM32 RNG). Every state-changing
  request carries it in the JSON body as `token` (a cross-site page cannot read or forge it).
  Sessions expire after 30 min idle; 4 sessions max.
- `SET_OFF` and `POWER_CYCLE` also need `password` in the body. 5 wrong passwords lock
  sign-in and re-authentication for 30 s (HTTP 429).
- Plain HTTP: cabinet LAN or router VPN only (S7-4).

## Hardware test (2026-11-23, Debug build, DIS_WDT fitted, 24 V connected; no OFF or power-cycle sent)

Boot: `CTRL: start latch=0 config=0 known=1 coil_supply=1 (no pending power-cycle)`.
API: no token → 401; wrong password → 403; correct → token; `SET_OFF` with wrong or missing
password → 403 (nothing applied); `SET_ON` CH1 → `APPLIED`; invalid type, mask 0 and 601 s →
400; `CANCEL_CYCLE` → `APPLIED`; unknown outcome id → 404; logout then reuse → 401; six wrong
logins → 403×5 then 429, correct password while locked → 429. Relay outputs stayed 0 and all
channels `ON`. Serial `GET` and `SET 0` returned `OK,0,0,0`. Heap minimum 22.3 KB; watchdog
`run`; no glitches.

## Not yet tested (needs someone at the board, only when energizing coils is safe)

1. Relay operation H1: web OFF / ON on one channel; listen or measure the contact.
2. Power-cycle 10 s: load off, back on after 10 s; outcome `APPLIED`; `cycle_remaining_s` counts down.
3. Supersede: start a 60 s cycle, then ON → load on at once, old request `SUPERSEDED`.
4. Reset during a cycle: start 60 s, reset the board → log `pending power-cycle ... resumed`, load
   comes back after the full 60 s from boot.
5. Rule (c): with a channel OFF, remove 24 V → log `24 V lost, released latches`, latch 0;
   restore 24 V → load stays on. Confirms H2 polarity at the same time.
6. Latency of rule (c) with a scope (PB3 edge → PCA IO pin LOW) ≤ 10 ms.
7. Glitch filter while switching relays rapidly (`control.glitches`).
