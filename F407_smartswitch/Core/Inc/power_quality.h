#ifndef POWER_QUALITY_H
#define POWER_QUALITY_H
#include "acquisition.h"
#include <stdbool.h>
#include <stdint.h>

/* Internal to the Acquisition task (single thread): streaming power-quality engine.
 * acquisition.c feeds every sample, calls Pq_HalfDone after each 64-sample half buffer
 * (= half a mains cycle when synced) and Pq_EndWindow after each 1 s window. */

typedef struct {
  int ref_line;                         /* -1 none */
  float dc_v[ACQ_AC_CHANNELS];          /* DC of (P-N) from the previous window, counts */
  float dc_i;                           /* CT DC from the previous window, counts */
  bool line_valid[ACQ_AC_CHANNELS];     /* previous window validity == VALID */
  uint32_t start_idx;                   /* absolute sample index of the window start */
} PqWindowSetup;

void Pq_Init(void);
void Pq_BeginWindow(const PqWindowSetup *setup);
void Pq_Sample(const int32_t d[ACQ_AC_CHANNELS], int32_t ct, uint32_t sampleIdx);
/* contiguous: this half directly follows the previous processed half. */
void Pq_HalfDone(bool contiguous, uint32_t nowMs);
/* n: samples in the window; fs: sample rate used (Hz); vRefRms/iRms in counts (DC removed);
 * ctValid: CT validity VALID. Fills out (keeps counters/energy/last event). */
void Pq_EndWindow(uint32_t n, float fs, float vRefRms, float iRms, bool ctValid,
    const int32_t lineDv[ACQ_AC_CHANNELS], AcqPq *out);
/* Timer auto-reload for the next window (sampling locked to 128 x f, or nominal). */
uint32_t Pq_NextArr(uint32_t nominalArr, uint32_t timerHz);

#endif
