# Phase 3: Acquisition service

Branch `feature/phase3-acquisition`, 2026-11-24. Implements
[tasks/acquisition.md](architecture/tasks/acquisition.md) for a board **without the AMC3330
voltage sensors** (owner: not populated yet; only the current sensor is fitted).

## What it does

- `Core/Src/acquisition.c` is the only owner of **ADC1, ADC2, TIM3 and DMA2 Stream0**.
  `adc_raw.c` (the Phase 0.5 temporary owner) is removed.
- ADC1 (master) and ADC2 (slave) run in **dual regular simultaneous** mode, triggered by
  **TIM3 TRGO at 6.4 kHz** (84 MHz / 13125), i.e. 128 samples per 50 Hz cycle.
  5 ranks per ADC, 480-cycle sampling on every channel (5 x 492 / 21 MHz = 117 us of
  the 156 us period). DMA mode 2 packs each pair in one word (ADC2 << 16 | ADC1),
  circular buffer of 2 x 64 samples in SRAM, half/full interrupts notify the task.

  | rank | ADC1 | ADC2 |
  |---|---|---|
  | 1 | IN10 PC0 vSense_P1 | IN12 PC2 vSense_N1 |
  | 2 | IN0 PA0 vSense_P2 | IN3 PA3 vSense_N2 |
  | 3 | IN4 PA4 vSense_P3 | IN5 PA5 vSense_N3 |
  | 4 | IN8 PB0 vSense_P4 | IN9 PB1 vSense_N4 |
  | 5 | IN6 PA6 CT (Vout_current_sense) | IN13 PC3 VBAT |

- Task `acq`: static, 384-word CCM stack, priority `osPriorityNormal1` (below Control).
  Every 1 s (100 half-buffers = 6400 samples) it computes:
  - AC line n: RMS of (P − N) → V = counts x 3.0/4096 x 200.1 (reported as V x10).
  - CT: RMS of the AC component → mA = counts x 7.33; DC mean reported as raw (bias check).
  - VBAT: mean of 6400 samples → mV with the same nominal formula as before.
- Validity per signal: `VALID / STALE / OUT_OF_RANGE / DIAG_FAIL / RAIL_OFF / NOT_FITTED / NO_DATA`.
  A number is published only when the state is `VALID` (web JSON prints `null` otherwise).
  `STALE` when the value is older than 2.5 s.
- **`ACQ_AMC_FITTED` (acquisition.h, default 0):** AC voltage channels always report
  `NOT_FITTED`. Reason: without the AMC3330 the ADC inputs float, and DIAG has a 10k pull-up
  to +3.3V_AMC (R201), so DIAG would read HIGH and fake a healthy channel if the rail were on.
  With 1: `RAIL_OFF` when PC10 is low, `NO_DATA` for 1 s after the rail turns on, `DIAG_FAIL`
  when DIAG is low or (P − N) < −2.0 V (AMC fail-safe), `OUT_OF_RANGE` when P or N hits a rail.
- Fault handling: no DMA interrupt for 50 ms, DMA error or ADC overrun → `running=false`,
  restart at most once per second (`restarts` counter, log `ACQ: restart (...)`).
  Half-buffers overwritten before processing are counted in `missed`.
- Users: web `/api/status` (`vbat` + new `acq` object) and serial `BATV`/`BACKUP` read
  the snapshot (`Acquisition_GetSnapshot`). Nobody else starts the ADC.

`MX_ADC2_Init()` still runs (CubeMX), which sets the ADC GPIOs to analog through
`HAL_ADC_MspInit`; Acquisition then reprograms the ADC registers directly and does not use
the HAL ADC state machine. ADC1 and TIM3 are not in the `.ioc` (clocks enabled in code);
`DMA2_Stream0_IRQHandler` is defined in `acquisition.c`.

### Deviation from S3-3

VBAT is a **regular rank (ADC2 rank 5) averaged over 1 s**, not an ADC1 injected
conversion. In dual regular simultaneous mode both ADCs must run the same sequence, and the
long 480-cycle sampling already suits the ~37 kOhm divider (10 nF at the pin). Averaging
6400 samples gives a steadier value than one injected sample.

### Not done (needs AMC3330)

Contact feedback into Control (`effective[]`), AC voltage calibration, DIAG/rail tests.

## Hardware test 2026-11-24

Board 1 (MAC 02:A2:C0:99:BD:88), DIS_WDT jumper fitted, 24 V present, no AC load, Debug build.

| Test | Result |
|---|---|
| Build | 0 errors, 0 warnings (headless clean build) |
| Boot | `ACQ: dual ADC 6400 Hz, AMC3330 not fitted`; PCA9536 latch kept (0 → 0) |
| Sampling rate | `cycles` grows ~50/s (≈ 6400 samples/s), `missed` 0 |
| VBAT | 3297 counts → 6.72 V (web and serial `BATV,3297,6719`); charger CHRG=0 (charging) |
| CT, no load | DC 2049 counts (≈1.50 V bias as designed), noise 29 mA rms |
| AC channels | `NOT_FITTED` on all four, no number shown |
| DMA stopped once via SWD (DMA2_Stream0 EN=0) | restart within ~1 s, `restarts` 1, values stay VALID |
| DMA held stopped for 6 s | `running=false`, VBAT/CT `STALE` after 2.5 s, restarts ~1/s, VALID again ~1 s after release |
| Serial `BACKUP` | `BACKUP,3297,0,1,1,1` |
| JSON size | 1751 B (buffer 3072) |
| Stack free (words) | acq 290, control 296, defaultTask 284; heap minimum 22.3 KB |

Not tested: CT against a clamp meter with a real AC load (owner at the bench), VBAT against
a meter on this board, long-duration run, AMC3330 channels.

The first flash attempt on board 2 failed (`failed to erase memory`) without the DIS_WDT
jumper: erasing the 128 KB sector 5 takes about 1–2 s and the STWD100 (1.12 s) resets the MCU
during the erase. Fit DIS_WDT before flashing an image larger than 128 KB.
