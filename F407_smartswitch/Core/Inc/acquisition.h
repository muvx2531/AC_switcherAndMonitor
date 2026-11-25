#ifndef ACQUISITION_H
#define ACQUISITION_H
#include <stdbool.h>
#include <stdint.h>

/* Acquisition service (docs/architecture/tasks/acquisition.md).
 * Sole owner of ADC1, ADC2, TIM3 and DMA2 Stream0. ADC1/ADC2 run in dual regular
 * simultaneous mode, triggered by TIM3 TRGO at 6.4 kHz (128 samples per 50 Hz cycle):
 *
 *   rank | ADC1 (master)        | ADC2 (slave)
 *   1    | IN10 PC0  vSense_P1  | IN12 PC2  vSense_N1
 *   2    | IN0  PA0  vSense_P2  | IN3  PA3  vSense_N2
 *   3    | IN4  PA4  vSense_P3  | IN5  PA5  vSense_N3
 *   4    | IN8  PB0  vSense_P4  | IN9  PB1  vSense_N4
 *   5    | IN6  PA6  CT (Vout_current_sense) | IN13 PC3 VBAT
 *
 * Board option: the AMC3330 isolated voltage sensors may be unpopulated. With
 * ACQ_AMC_FITTED 0 the AC voltage channels report NOT_FITTED and never a number. */
#ifndef ACQ_AMC_FITTED
#define ACQ_AMC_FITTED 1
#endif

#define ACQ_AC_CHANNELS 4U

typedef enum {
  ACQ_NO_DATA = 0,   /* nothing measured yet */
  ACQ_VALID,
  ACQ_STALE,         /* sampling stopped: last value is too old */
  ACQ_OUT_OF_RANGE,  /* ADC input at a rail */
  ACQ_DIAG_FAIL,     /* AMC3330 DIAG low or fail-safe output */
  ACQ_RAIL_OFF,      /* +3.3V_AMC rail is off */
  ACQ_NOT_FITTED,    /* sensor not populated on this board */
} AcqValidity;

typedef struct {
  AcqValidity validity;
  int32_t value;       /* scaled value; meaningless unless validity == ACQ_VALID */
  uint16_t raw;        /* see field comments in AcqSnapshot */
  uint32_t age_ms;     /* since the value was computed */
} AcqSignal;

/* AC power quality (docs/ac-quality.md). Indicative values, not IEC 61000-4-30 class A.
 * The CT is on the board feed, so current and power are the total of all four outputs;
 * voltage comes from a "reference" line (first line VALID and > 50 V). */
#define ACQ_PQ_HARMONICS 25U          /* harmonics 1..25 computed; 2..25 published */
#define ACQ_NOMINAL_V    230.0f
#ifndef ACQ_CT_PHASE_DEG
#define ACQ_CT_PHASE_DEG 0.0f         /* CT + filter phase lead at 50 Hz (calibration) */
#endif

typedef enum { PQ_EV_NONE = 0, PQ_EV_SAG, PQ_EV_SWELL, PQ_EV_INTERRUPTION } AcqPqEvent;
typedef enum { PQ_CONTACT_UNKNOWN = 0, PQ_CONTACT_OK, PQ_CONTACT_NO_VOLTAGE, PQ_CONTACT_STUCK_ON } AcqPqContact;

typedef struct {
  AcqPqEvent type;
  uint32_t start_ms;       /* uptime at start */
  uint32_t duration_ms;    /* so far if still active */
  uint16_t extreme_dv;     /* min (sag/interruption) or max (swell) half-cycle RMS, V x10 */
  bool active;
  bool open_ended;         /* measurement stopped (e.g. AMC rail off) before the event ended */
} AcqPqEventRecord;

typedef struct {
  bool freq_valid;
  uint32_t freq_mhz;       /* mains frequency, mHz */
  bool synced;             /* sampling locked to 128 x mains frequency */
  int8_t ref_line;         /* 0..3, -1 none */
  bool v_valid;
  uint16_t v_thd_x10;      /* THD-F % x10 (harmonics 2..25 vs fundamental) */
  uint16_t v_crest_x100;
  uint16_t v_h_x10[ACQ_PQ_HARMONICS - 1U];   /* harmonic 2..25, % of fundamental x10 */
  bool i_valid;            /* needs >= 0.1 A */
  uint16_t i_thd_x10;
  uint16_t i_crest_x100;
  uint16_t i_h_x10[ACQ_PQ_HARMONICS - 1U];
  bool p_valid;
  int32_t p_dw, s_dva, q_dvar;   /* 0.1 W / VA / var; Q = fundamental, + inductive */
  bool pf_valid;           /* needs >= 0.1 A */
  int16_t pf_x1000, dpf_x1000;   /* true PF = P/S, displacement PF = cos(phi1) */
  uint64_t energy_uwh;     /* since boot (RAM only until Storage) */
  bool ct_reversed;        /* P < -5 W for 3 s: the cabinet has no generation, so the CT is clamped backwards */
  bool events_active;      /* at least one line is commanded ON and measured */
  uint16_t supply_min_dv, supply_max_dv;   /* half-cycle RMS min/max over the last window */
  uint32_t sag_count, swell_count, interruption_count;
  AcqPqEventRecord last;
  AcqPqContact contact[ACQ_AC_CHANNELS];
} AcqPq;

typedef struct {
  bool running;
  uint32_t restarts;       /* sampling restarts after a stall or DMA/ADC error */
  uint32_t missed_halves;  /* DMA half-buffers overwritten before processing */
  uint32_t cycles;         /* 128-sample windows processed since boot */
  AcqSignal ac_v[ACQ_AC_CHANNELS]; /* value: V rms x10 over 1 s; raw: rms of (P-N) in counts */
  AcqSignal ct;            /* value: mA rms over 1 s (uncalibrated); raw: DC mean in counts */
  AcqSignal vbat;          /* value: mV, mean over 1 s (uncalibrated); raw: mean counts */
  uint8_t diag[ACQ_AC_CHANNELS];   /* raw DIAG_Sx levels (PD8/PD10/PD12/PD14) */
  uint8_t cal_version;     /* 0 = nominal constants, not calibrated */
  AcqPq pq;
} AcqSnapshot;

/* Call once from the default task after the RTOS is running. */
bool Acquisition_Start(void);
/* Any task. Copies the latest snapshot and fills age/STALE. */
void Acquisition_GetSnapshot(AcqSnapshot *out);
const char *Acquisition_ValidityName(AcqValidity v);
const char *Acquisition_EventName(AcqPqEvent e);
const char *Acquisition_ContactName(AcqPqContact c);

#endif
