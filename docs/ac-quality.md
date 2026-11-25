# AC power quality (Acquisition extension)

Branch `feature/ac-quality`, 2026-11-25. Owner request: report AC quality (harmonic
distortion, power consumption and other values the current hardware supports).

Hardware basis: four AMC3330 line-voltage channels after the relays and one SCT-013-010 CT
on the **board feed** (owner: everything leaving the board is ours), so current, power and
energy are the **total of all four outputs**. All values are indicative (not IEC 61000-4-30
class A); nominal 230 V.

## What is computed (`power_quality.c`, called by `acquisition.c`)

| Value | Method |
|---|---|
| Frequency | interpolated positive zero crossings of the reference line over 1 s (hysteresis ~22 V) |
| Synced sampling | TIM3 ARR set every second so the sample rate is 128 × f (ARR preload, glitch-free); then a 1 s window is a whole number of cycles and each 64-sample DMA half is half a cycle |
| Reference line | first line VALID and > 50 V in the previous window (all lines share one supply) |
| Harmonics 2–25, THD-F | DFT at bins 1..25 of the reference voltage and of the CT current, 1 s window; % of the fundamental |
| Crest factor | peak / RMS (DC removed), voltage and current |
| P | Σ over harmonics 1..25 of ½ Re(V·I*), current phase corrected for the ADC rank delay (reference rank vs CT rank 5, 23.4 µs per rank) and `ACQ_CT_PHASE_DEG` (default 0) |
| S, Q, PF, DPF | S = Vrms·Irms (true RMS); Q = fundamental reactive power (+ inductive, − capacitive); PF = P/S; DPF = cos φ1 |
| Energy | Σ max(P, 0)·t since boot, RAM only (persisted after the Storage phase) |
| Supply events | one-cycle RMS refreshed every half cycle on the highest line that Control reports ON (NC contact closed) and that was VALID: sag < 90 %, swell > 110 %, interruption < 10 % of 230 V, hysteresis 2 %. A sag that falls below 10 % becomes an interruption; a dip ends above 92 %. Counts, last event (type, uptime start, duration, min/max V, ongoing, "measurement stopped" when no line can be measured any more, e.g. AMC rail off on battery) |
| Contact check | per channel, Control's effective state vs measured line voltage (> 50 V = live): ON but dead while another ON line shows supply → `NO_VOLTAGE`; OFF but live → `STUCK_ON`; must persist 2 windows |
| CT reversed | P < −5 W for 3 s → `ct_reversed` (the cabinet has no generation) |

THD-I, current crest factor and PF are published only above 0.1 A (below that it is CT noise).
CPU cost: ~100 multiply-adds per sample (≈ 1–2 %); no extra sample buffers.

## API and page

`/api/status` → `acq.pq`: `freq_mhz`, `synced`, `ref_line`, `v_thd_x10`, `v_crest_x100`,
`v_h_x10[24]` (h2..h25), `i_thd_x10`, `i_crest_x100`, `i_h_x10[24]`, `p_dw`, `s_dva`, `q_dvar`,
`pf_x1000`, `dpf_x1000`, `energy_mwh`, `ct_reversed`, `events{active, supply_min_dv,
supply_max_dv, sag, swell, interruption, last{type, start_ms, duration_ms, extreme_dv, active,
open_ended}}`, `contact[4]` (`OK / NO_VOLTAGE / STUCK_ON / UNKNOWN`). Unavailable values are
`null`. JSON buffer raised to 4096 B (status ≈ 2.5 KB).

Page: "Power quality" and "Harmonics / Supply events" cards; relay cards show the line voltage
and the contact check.

## Bench test 2026-11-25 (board 2, AMC3330 fitted)

| Test | Result |
|---|---|
| No load | f 49.97–50.01 Hz, synced; V THD 0.5 % (h2 0.3, h3 0.3, others ≤ 0.1 %), V crest 1.45; I 27 mA noise, P 0.2 W; no events; contacts OK (CH2 OFF read 3 V) |
| Owner's load (switch-mode supply), CT first clamped backwards | P −64.6 W, cos φ −0.945 → CT reversed (warning added) |
| Same load, CT turned around | I 0.57–0.58 A (owner's clamp meter 0.56 A), P +65–66 W, S 131–134 VA, PF 0.49–0.50, DPF 0.945, Q −22.6 var (capacitive), THD-I 160 %, h3 92 %, h5 81 %, h7 67 %, h9 51 % (typical rectifier input without PFC) |
| Energy | counts up with P (e.g. 1.5 Wh after ~85 s at 65 W) |

Not verified yet: P/PF against a power meter, CT phase calibration with a resistive load
(DPF should read 1.000; adjust `ACQ_CT_PHASE_DEG`), V THD against a reference instrument
(0.5 % is lower than typical mains), sag/swell/interruption with a variac or by switching the
supply, contact `STUCK_ON` / `NO_VOLTAGE` faults (need a fault or simulation).

Development note: each `STM32_Programmer_CLI ... mode=UR ... -rst` flash resets the MCU twice
(connect under reset + `-rst`), so three flashes within 10 min reach the safe-mode threshold
(5 resets). Clear with `relay-command.ps1 -Command CLRSAFE`, or flash without `-rst`.
