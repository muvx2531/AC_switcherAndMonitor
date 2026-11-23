# Web UI and HTTP API (draft)

Design draft 2026-11-23; firmware side not implemented. The page has four tabs:

| Tab | Available when | Notes |
|---|---|---|
| Status | Phase 0.5 (read-only status web) | raw readings only, no login |
| Relays | after the Control service (Phase 2) | commands through `Control_Submit`; disabled while `control.available` is false or absent |
| Network | Phase 6 | DHCP / static with safe apply and automatic revert |
| Firmware | Phase 5 (bootloader + storage partitions) | FOTA draft only |

## Files

- `F407_smartswitch/web/index.html`: single self-contained page (HTML + CSS + JS, no
  external resources), ~28 KB raw / ~10 KB gzip. Embedded in flash and served at `/`.
- Opened outside HTTP (file preview) or with `?mock`, it uses a built-in mock API
  (mock password `demo`) and shows a "Mock data" badge. Served by the board it polls
  `GET /api/status` every 1 s, times out after 2.5 s and shows "Disconnected" after two failures.
- UI language: English. Primary device: laptop (responsive down to phone width).

## Authentication (proposal)

- One password per board (stored as a salted hash in config; unique default or forced change).
- `POST /api/login {password}` → `{csrf}` and a session cookie (`HttpOnly`, `SameSite=Strict`).
  Every state-changing request sends `X-CSRF-Token`. Failed logins are rate limited (HTTP 429).
- Re-authentication: requests that remove power from equipment (`SET_OFF`, `POWER_CYCLE`),
  network changes and firmware apply also carry `password` in the body. `SET_ON`
  (the safe direction) needs only the session.
- Plain HTTP sends the password unencrypted: acceptable only on the cabinet LAN or through
  the router VPN (decision S7-4). Never port-forward the board.

## Control and configuration endpoints (proposal)

| Method / path | Body | Response | Errors |
|---|---|---|---|
| `POST /api/relay` | `{type: SET_ON\|SET_OFF\|POWER_CYCLE\|CANCEL_CYCLE, channel_mask, duration_s, password?}` | `{request_id}` | 403 bad password, 409 `{reason: NO_COIL_SUPPLY\|STATE_UNKNOWN}`, 400 `INVALID`, 503 queue busy |
| `GET /api/relay/outcome?id=` | — | `{state: PENDING\|APPLIED\|FAILED\|SUPERSEDED\|UNKNOWN}` | 404 expired |
| `POST /api/network` | `{mode: dhcp\|static, ip, mask, gw, dns, password}` | `{}` then the board switches | 403, 400 invalid |
| `POST /api/network/confirm` | — (sent from the new address) | `{}` | 409 nothing pending |
| `GET /api/fw/status` | — | `{running:{version,build,confirmed}, bootloader, slot:{state,version,size,check}, step, failed, history[]}` | — |
| `POST /api/fw/upload` | raw image stream | progress via `fw/status` | 403, 413 too large, 422 verify failed |
| `POST /api/fw/apply` | `{password}` | `{}` then reboot | 403, 409 slot not valid |

Network safe apply: the board applies the new settings, keeps the previous ones for 120 s
and reverts unless `POST /api/network/confirm` arrives on the new address. Serial remains
the fallback for recovery.

Firmware update steps shown on the page: Upload → Verify (size, CRC, signature) → Ready →
Reboot → Boot check → Committed (or rolled back by the bootloader). Relay states are kept
across the reboot (invariants I1–I4).

`/api/status` additionally carries, once Control exists:
`"control": {"available": true, "coil_supply": true, "cycle_remaining_s": [0,0,0,0]}`.

## `GET /api/status`

`Content-Type: application/json`, `Cache-Control: no-store`. Every field is a raw reading
or driver state; the page labels raw values and does not interpret polarity.

```json
{
  "fw":    { "build": "string", "uptime_ms": 8500 },
  "net":   { "link": true, "mode": "dhcp|static", "ip": "192.168.1.10",
             "mask": "255.255.255.0", "gw": "192.168.1.1",
             "mac": "06:3A:1F:52:9C:04", "phy_id": "0007:C131" },
  "relay": { "ok": true, "err": 0, "output": 2, "input": 242, "config": 240 },
  "power": { "vin_detect": 1, "efuse_ilim": 1, "chrg": 1, "fault": 1 },
  "vbat":  { "ok": true, "raw": 3207, "mv_est": 6553 },
  "amc_diag": [1, 1, 1, 0],
  "flash": [ { "ok": true, "jedec": "EF 40 18" }, { "ok": true, "jedec": "EF 40 18" } ],
  "sys":   { "heap_free": 24800, "heap_min": 15912, "heap_total": 32768,
             "log": { "queued": 0, "high": 412, "dropped": 0,
                      "dma_start_err": 0, "dma_xfer_err": 0 },
             "tasks": [ { "name": "defaultTask", "prio": 24, "stack_free": 180 } ] }
}
```

| Field | Source today | Notes |
|---|---|---|
| `relay.output/input/config` | `PCA9536_ReadRegisters` (read-only, through the I2C bus mutex) | numbers; bit n = CH(n+1). `config` bit 1 = pin is input. `ok=false` + `err` (I2cBusResult) when the read fails; page shows UNKNOWN |
| `power.*` | GPIO levels PB3, PB9, PE13, PE11 | 0/1 or `null` if unavailable |
| `vbat.raw`, `vbat.mv_est` | ADC2 rank 9 (IN13), estimate as in `backup-command.md` | needs one ADC owner shared by serial `BACKUP/BATV` and the web |
| `amc_diag` | GPIO PD8/PD10/PD12/PD14 | `null` entries allowed |
| `flash[]` | JEDEC ID read once at boot via `w25q128` driver | cached, not re-read per request |
| `net.*` | CycloneTCP interface + PHY probe | `mode` becomes `dhcp` when the DHCP lease is used |
| `sys.*` | `xPortGetFreeHeapSize`, `xPortGetMinimumEverFreeHeapSize`, `uxTaskGetSystemState`, `DebugLog_GetStats` | `stack_free` in words |

## Firmware plan (next step, not started)

1. Enable `HTTP_SERVER_SUPPORT` (CycloneTCP), 1–2 connections, measure heap; raise
   `configTOTAL_HEAP_SIZE` through the `.ioc` only if needed.
2. Single temporary ADC owner module used by both serial and web.
3. DHCP with static fallback `192.168.0.223/24` when no lease within ~10 s; MAC from STM32 UID.
4. Remove the TCP echo test from the build path used by the web build.
5. No login in this bench-only phase (direct cable / isolated LAN); login is Phase 6.
