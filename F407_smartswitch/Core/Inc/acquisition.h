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
#define ACQ_AMC_FITTED 0
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
} AcqSnapshot;

/* Call once from the default task after the RTOS is running. */
bool Acquisition_Start(void);
/* Any task. Copies the latest snapshot and fills age/STALE. */
void Acquisition_GetSnapshot(AcqSnapshot *out);
const char *Acquisition_ValidityName(AcqValidity v);

#endif
