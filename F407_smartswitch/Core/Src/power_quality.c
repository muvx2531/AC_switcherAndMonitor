#include "power_quality.h"
#include "control.h"
#include "storage.h"
#include "main.h"
#include <math.h>
#include <string.h>

/* Streaming power-quality engine (docs/ac-quality.md).
 *
 * Sampling is locked to 128 samples per mains cycle (Pq_NextArr adjusts TIM3), so a
 * 1 s window holds a whole number of cycles and a plain DFT at bins 1..25 gives the
 * harmonics without leakage, and each 64-sample DMA half is exactly half a cycle
 * (IEC 61000-4-30 style "RMS over one cycle refreshed every half cycle"). */

#define N_CYCLE          128U
#define H                ACQ_PQ_HARMONICS
#define V_PER_COUNT      (3.0f / 4096.0f * 200.1f)   /* line volts per (P-N) count */
#define A_PER_COUNT      0.00733f                     /* CT amps per count (SCT-013-010) */
#define ADC_CLK_HZ       21000000.0f
#define RANK_CYCLES      492.0f                       /* 480 sampling + 12 conversion */
#define ZC_HYST_COUNTS   150.0f                       /* ~22 V: arm the next zero crossing */
#define MIN_I_FOR_SHAPE  0.1f                         /* A: below this THD-I and PF are noise */
#define LIVE_V           50.0f                        /* a line above this has voltage */
#define SUPPLY_V         180.0f                       /* a commanded-ON line reads supply */
#define SAG_START        (0.90f * ACQ_NOMINAL_V)
#define SWELL_START      (1.10f * ACQ_NOMINAL_V)
#define INTR_START       (0.10f * ACQ_NOMINAL_V)
#define HYST             (0.02f * ACQ_NOMINAL_V)
#define CONTACT_PERSIST  2U                           /* windows */

static float cosT[N_CYCLE], sinT[N_CYCLE];

typedef struct {
  float vRe[H + 1], vIm[H + 1], iRe[H + 1], iIm[H + 1];
  float vPeak, iPeak;
  uint32_t zc;
  float zcFirst, zcLast, prevV;
  bool armed;
  float hs[ACQ_AC_CHANNELS];          /* sum of squares in the current half */
  float supplyMin, supplyMax;
} Acc;

static PqWindowSetup ws;
static Acc acc;
static float hsPrev[ACQ_AC_CHANNELS];
static bool hsPrevValid;
static float lastFreq;
static bool freqValid;

/* events */
static AcqPqEvent evState;
static AcqPqEventRecord evRec;
static uint32_t sagCount, swellCount, intrCount;
static bool eventsActive;
static uint64_t energyUwh;
static uint8_t negativeWindows;
static uint8_t contactMismatch[ACQ_AC_CHANNELS];
static AcqPqContact contactState[ACQ_AC_CHANNELS];

void Pq_Init(void)
{
  for (unsigned int n = 0; n < N_CYCLE; n++) {
    cosT[n] = cosf(2.0f * 3.14159265f * (float)n / (float)N_CYCLE);
    sinT[n] = sinf(2.0f * 3.14159265f * (float)n / (float)N_CYCLE);
  }
  memset(&acc, 0, sizeof(acc));
  ws.ref_line = -1;
  hsPrevValid = false;
}

void Pq_BeginWindow(const PqWindowSetup *setup)
{
  ws = *setup;
  memset(&acc, 0, sizeof(acc));
  acc.supplyMin = 1e9f;
}

void Pq_Sample(const int32_t d[ACQ_AC_CHANNELS], int32_t ct, uint32_t sampleIdx)
{
  unsigned int n = sampleIdx & (N_CYCLE - 1U);
  float i = (float)ct - ws.dc_i;
  float ai = fabsf(i);
  if (ai > acc.iPeak) acc.iPeak = ai;
  for (unsigned int ch = 0; ch < ACQ_AC_CHANNELS; ch++) {
    float x = (float)d[ch] - ws.dc_v[ch];
    acc.hs[ch] += x * x;
  }
  if (ws.ref_line >= 0) {
    float v = (float)d[ws.ref_line] - ws.dc_v[ws.ref_line];
    float av = fabsf(v);
    if (av > acc.vPeak) acc.vPeak = av;
    for (unsigned int k = 1; k <= H; k++) {
      unsigned int idx = (k * n) & (N_CYCLE - 1U);
      float c = cosT[idx], s = sinT[idx];
      acc.vRe[k] += v * c; acc.vIm[k] += v * s;
      acc.iRe[k] += i * c; acc.iIm[k] += i * s;
    }
    /* positive-going zero crossing with hysteresis, interpolated */
    if (v < -ZC_HYST_COUNTS) acc.armed = true;
    if (acc.armed && acc.prevV < 0.0f && v >= 0.0f) {
      float t = (float)(sampleIdx - ws.start_idx) - 1.0f + acc.prevV / (acc.prevV - v);
      if (acc.zc == 0U) acc.zcFirst = t;
      acc.zcLast = t;
      acc.zc++;
      acc.armed = false;
    }
    acc.prevV = v;
  } else {
    for (unsigned int k = 1; k <= H; k++) {
      unsigned int idx = (k * n) & (N_CYCLE - 1U);
      acc.iRe[k] += i * cosT[idx]; acc.iIm[k] += i * sinT[idx];
    }
  }
}

static void closeEvent(bool openEnded)
{
  if (evState == PQ_EV_NONE) return;
  Storage_LogEvent(ST_EV_PQ, (int32_t)evRec.type, (int32_t)evRec.duration_ms, (int32_t)evRec.extreme_dv, 0);
  evRec.active = false;
  evRec.open_ended = openEnded;
  evState = PQ_EV_NONE;
}

static AcqPqEventRecord prevRec;   /* restored when an event is cancelled */

/* An event that started just before the AMC rail was switched off is a measurement
 * artefact (the outputs collapse to 0 V): forget it instead of logging it. */
static void cancelEvent(void)
{
  if (evState == PQ_EV_NONE) return;
  if (evRec.type == PQ_EV_SAG && sagCount) sagCount--;
  else if (evRec.type == PQ_EV_SWELL && swellCount) swellCount--;
  else if (intrCount) intrCount--;
  evRec = prevRec;
  evState = PQ_EV_NONE;
}

static void startEvent(AcqPqEvent type, float v, uint32_t nowMs)
{
  evState = type;
  prevRec = evRec;
  memset(&evRec, 0, sizeof(evRec));
  evRec.type = type;
  evRec.start_ms = nowMs;
  evRec.active = true;
  evRec.extreme_dv = (uint16_t)(v * 10.0f + 0.5f);
  if (type == PQ_EV_SAG) sagCount++;
  else if (type == PQ_EV_SWELL) swellCount++;
  else intrCount++;
}

void Pq_HalfDone(bool contiguous, uint32_t nowMs)
{
  float urms[ACQ_AC_CHANNELS];
  bool ok = contiguous && hsPrevValid;
  for (unsigned int ch = 0; ch < ACQ_AC_CHANNELS; ch++) {
    urms[ch] = sqrtf((hsPrev[ch] + acc.hs[ch]) / (float)N_CYCLE) * V_PER_COUNT;
    hsPrev[ch] = acc.hs[ch];
    acc.hs[ch] = 0.0f;
  }
  hsPrevValid = true;
  if (!ok) return;

  /* AMC rail off (battery, or not fitted): nothing can be measured. */
  bool railOn = Acquisition_AmcFitted() && (En_3v3_AMC_GPIO_Port->ODR & En_3v3_AMC_Pin) != 0U;
  if (!railOn) {
    if (evState != PQ_EV_NONE && nowMs - evRec.start_ms < 200U) cancelEvent();
    else closeEvent(true);
    eventsActive = false;
    hsPrevValid = false;
    return;
  }

  /* Supply voltage = highest line that Control has ON (NC contact closed) and that was
   * measured VALID in the previous window. With no such line the supply is unknown. */
  ControlSnapshot c;
  Control_GetSnapshot(&c);
  float supply = -1.0f;
  for (unsigned int ch = 0; ch < ACQ_AC_CHANNELS; ch++)
    if (ws.line_valid[ch] && c.available && c.effective[ch] == EFF_ON && urms[ch] > supply) supply = urms[ch];
  eventsActive = supply >= 0.0f;
  if (!eventsActive) { closeEvent(true); return; }

  if (supply < acc.supplyMin) acc.supplyMin = supply;
  if (supply > acc.supplyMax) acc.supplyMax = supply;

  switch (evState) {
    case PQ_EV_NONE:
      if (supply < INTR_START) startEvent(PQ_EV_INTERRUPTION, supply, nowMs);
      else if (supply < SAG_START) startEvent(PQ_EV_SAG, supply, nowMs);
      else if (supply > SWELL_START) startEvent(PQ_EV_SWELL, supply, nowMs);
      break;
    case PQ_EV_SAG:
    case PQ_EV_INTERRUPTION:
      /* One dip event, classified by its depth: a sag that falls below the interruption
       * threshold becomes an interruption. It ends when the supply is back above the
       * sag threshold plus hysteresis. */
      if (evState == PQ_EV_SAG && supply < INTR_START) {
        sagCount--; intrCount++; evState = evRec.type = PQ_EV_INTERRUPTION;
      }
      if ((uint16_t)(supply * 10.0f) < evRec.extreme_dv) evRec.extreme_dv = (uint16_t)(supply * 10.0f + 0.5f);
      if (supply > SAG_START + HYST) closeEvent(false);
      break;
    case PQ_EV_SWELL:
      if ((uint16_t)(supply * 10.0f) > evRec.extreme_dv) evRec.extreme_dv = (uint16_t)(supply * 10.0f + 0.5f);
      if (supply < SWELL_START - HYST) closeEvent(false);
      break;
  }
  if (evState != PQ_EV_NONE) evRec.duration_ms = nowMs - evRec.start_ms + 10U;
}

static uint16_t pctX10(float part, float whole)
{
  if (whole <= 0.0f) return 0U;
  float p = part / whole * 1000.0f;
  return p > 65535.0f ? 65535U : (uint16_t)(p + 0.5f);
}

static void updateContacts(const int32_t lineDv[ACQ_AC_CHANNELS])
{
  ControlSnapshot c;
  Control_GetSnapshot(&c);
  bool supplySeen = false;
  for (unsigned int ch = 0; ch < ACQ_AC_CHANNELS; ch++)
    if (ws.line_valid[ch] && lineDv[ch] >= 0 && (float)lineDv[ch] / 10.0f > SUPPLY_V) supplySeen = true;
  for (unsigned int ch = 0; ch < ACQ_AC_CHANNELS; ch++) {
    AcqPqContact now = PQ_CONTACT_UNKNOWN;
    if (ws.line_valid[ch] && lineDv[ch] >= 0 && c.available && c.state_known) {
      bool live = (float)lineDv[ch] / 10.0f > LIVE_V;
      if (c.effective[ch] == EFF_ON) now = live ? PQ_CONTACT_OK : (supplySeen ? PQ_CONTACT_NO_VOLTAGE : PQ_CONTACT_UNKNOWN);
      else if (c.effective[ch] == EFF_OFF) now = live ? PQ_CONTACT_STUCK_ON : PQ_CONTACT_OK;
    }
    if (now == PQ_CONTACT_NO_VOLTAGE || now == PQ_CONTACT_STUCK_ON) {
      if (contactMismatch[ch] < CONTACT_PERSIST) contactMismatch[ch]++;
      if (contactMismatch[ch] < CONTACT_PERSIST) continue;   /* keep the previous state for now */
    } else {
      contactMismatch[ch] = 0U;
    }
    if (now != contactState[ch] && (now == PQ_CONTACT_NO_VOLTAGE || now == PQ_CONTACT_STUCK_ON ||
        contactState[ch] == PQ_CONTACT_NO_VOLTAGE || contactState[ch] == PQ_CONTACT_STUCK_ON))
      Storage_LogEvent(ST_EV_CONTACT, (int32_t)ch, (int32_t)now, 0, 0);
    contactState[ch] = now;
  }
}

void Pq_EndWindow(uint32_t n, float fs, float vRefRms, float iRms, bool ctValid,
    const int32_t lineDv[ACQ_AC_CHANNELS], AcqPq *out)
{
  /* frequency from interpolated zero crossings */
  freqValid = false;
  if (ws.ref_line >= 0 && acc.zc >= 20U && acc.zcLast > acc.zcFirst) {
    float f = (float)(acc.zc - 1U) * fs / (acc.zcLast - acc.zcFirst);
    if (f > 45.0f && f < 65.0f) { lastFreq = f; freqValid = true; }
  }
  out->freq_valid = freqValid;
  out->freq_mhz = freqValid ? (uint32_t)(lastFreq * 1000.0f + 0.5f) : 0U;
  out->ref_line = (int8_t)ws.ref_line;
  out->synced = freqValid && fabsf(fs / (float)N_CYCLE - lastFreq) < 0.02f;

  float scale = 2.0f / (float)n;           /* DFT bin -> peak amplitude */
  float vA[H + 1], iA[H + 1];
  for (unsigned int k = 1; k <= H; k++) {
    vA[k] = scale * sqrtf(acc.vRe[k] * acc.vRe[k] + acc.vIm[k] * acc.vIm[k]);
    iA[k] = scale * sqrtf(acc.iRe[k] * acc.iRe[k] + acc.iIm[k] * acc.iIm[k]);
  }

  /* voltage shape */
  out->v_valid = ws.ref_line >= 0 && vA[1] > 0.0f;
  float hv = 0.0f, hi = 0.0f;
  for (unsigned int k = 2; k <= H; k++) {
    hv += vA[k] * vA[k]; hi += iA[k] * iA[k];
    out->v_h_x10[k - 2U] = out->v_valid ? pctX10(vA[k], vA[1]) : 0U;
  }
  out->v_thd_x10 = out->v_valid ? pctX10(sqrtf(hv), vA[1]) : 0U;
  out->v_crest_x100 = (out->v_valid && vRefRms > 0.0f) ? (uint16_t)(acc.vPeak / vRefRms * 100.0f + 0.5f) : 0U;

  /* current shape */
  float irmsA = iRms * A_PER_COUNT;
  out->i_valid = ctValid && irmsA >= MIN_I_FOR_SHAPE && iA[1] > 0.0f;
  for (unsigned int k = 2; k <= H; k++) out->i_h_x10[k - 2U] = out->i_valid ? pctX10(iA[k], iA[1]) : 0U;
  out->i_thd_x10 = out->i_valid ? pctX10(sqrtf(hi), iA[1]) : 0U;
  out->i_crest_x100 = (out->i_valid && iRms > 0.0f) ? (uint16_t)(acc.iPeak / iRms * 100.0f + 0.5f) : 0U;

  /* power: per harmonic, current phase corrected for the sampling delay between the
   * reference line rank and the CT rank (5) and for the CT phase lead. */
  out->p_valid = ws.ref_line >= 0 && ctValid;
  out->pf_valid = false;
  if (out->p_valid) {
    float f = freqValid ? lastFreq : fs / (float)N_CYCLE;
    float tau = (float)(4 - ws.ref_line) * RANK_CYCLES / ADC_CLK_HZ;
    float theta1 = 2.0f * 3.14159265f * f * tau + Acquisition_CtPhaseDeg() * 3.14159265f / 180.0f;
    float p = 0.0f, q1 = 0.0f, ang1 = 0.0f;
    for (unsigned int k = 1; k <= H; k++) {
      /* X = scale * (Re - j Im) */
      float vr = scale * acc.vRe[k], vi = -scale * acc.vIm[k];
      float ir = scale * acc.iRe[k], ii = -scale * acc.iIm[k];
      float th = (float)k * theta1, ct = cosf(th), st = sinf(th);
      float cr = ir * ct + ii * st, ci = ii * ct - ir * st;   /* I * e^{-j th} */
      p += 0.5f * (vr * cr + vi * ci);                       /* Re(V conj(I)) / 2 */
      if (k == 1U) { q1 = 0.5f * (vi * cr - vr * ci); ang1 = atan2f(vi, vr) - atan2f(ci, cr); }
    }
    float pw = p * V_PER_COUNT * A_PER_COUNT;
    float sva = vRefRms * V_PER_COUNT * irmsA;
    float qv = q1 * V_PER_COUNT * A_PER_COUNT;
    out->p_dw = (int32_t)lroundf(pw * 10.0f);
    out->s_dva = (int32_t)lroundf(sva * 10.0f);
    out->q_dvar = (int32_t)lroundf(qv * 10.0f);
    if (ctValid && irmsA >= MIN_I_FOR_SHAPE && sva > 1.0f) {
      float pf = pw / sva;
      if (pf > 1.0f) pf = 1.0f;
      if (pf < -1.0f) pf = -1.0f;
      out->pf_valid = true;
      out->pf_x1000 = (int16_t)lroundf(pf * 1000.0f);
      out->dpf_x1000 = (int16_t)lroundf(cosf(ang1) * 1000.0f);
    }
    if (pw > 0.0f) energyUwh += (uint64_t)(pw * ((float)n / fs) * 1e6f / 3600.0f + 0.5f);
    if (pw < -5.0f) { if (negativeWindows < 255U) negativeWindows++; }
    else if (pw > -1.0f) negativeWindows = 0U;
  }
  bool rev = negativeWindows >= 3U;
  if (rev != out->ct_reversed) Storage_LogEvent(ST_EV_CT_REVERSED, rev ? 1 : 0, 0, 0, 0);
  out->ct_reversed = rev;
  out->energy_uwh = energyUwh;

  /* events summary */
  out->events_active = eventsActive;
  out->supply_min_dv = acc.supplyMin < 1e8f ? (uint16_t)(acc.supplyMin * 10.0f + 0.5f) : 0U;
  out->supply_max_dv = (uint16_t)(acc.supplyMax * 10.0f + 0.5f);
  out->sag_count = sagCount; out->swell_count = swellCount; out->interruption_count = intrCount;
  out->last = evRec;

  updateContacts(lineDv);
  memcpy(out->contact, contactState, sizeof(contactState));
}

void Pq_SetEnergy(uint64_t uwh) { energyUwh = uwh; }
uint64_t Pq_Energy(void) { return energyUwh; }

uint32_t Pq_NextArr(uint32_t nominalArr, uint32_t timerHz)
{
  if (!freqValid) return nominalArr;
  float arr = (float)timerHz / ((float)N_CYCLE * lastFreq) - 1.0f;
  uint32_t a = (uint32_t)(arr + 0.5f);
  uint32_t lo = nominalArr - nominalArr / 10U, hi = nominalArr + nominalArr / 10U;
  return a < lo ? lo : a > hi ? hi : a;
}
