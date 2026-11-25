#include "pq_history.h"
#include "storage.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

/* Ring in CCM, NOLOAD section (.ccmbss): not zeroed at boot, so head/count live in
 * normal .bss and only entries < count are ever read. */
static HistEntry ring[HIST_LEN] __attribute__((section(".ccmbss")));
static uint32_t head, count;          /* head = next write position */
static uint32_t newestEndS;

typedef struct {
  int32_t vSum, iSum, pSum, fSum, pfSum, vtSum, itSum;
  uint16_t vN, iN, pN, fN, pfN, vtN, itN;
  int16_t vMin, vMax;
  uint16_t seconds;
} MinuteAcc;

static MinuteAcc m;

static int16_t avg(int32_t sum, uint16_t n)
{
  if (n == 0U) return HIST_MISSING;
  int32_t a = (sum >= 0 ? sum + n / 2 : sum - n / 2) / (int32_t)n;
  if (a > INT16_MAX) a = INT16_MAX;
  if (a < INT16_MIN + 1) a = INT16_MIN + 1;
  return (int16_t)a;
}

static int16_t clamp16(int32_t v)
{
  return (int16_t)(v > INT16_MAX ? INT16_MAX : v < INT16_MIN + 1 ? INT16_MIN + 1 : v);
}

void History_AddSecond(const AcqSnapshot *s)
{
  if (m.seconds == 0U) { memset(&m, 0, sizeof(m)); m.vMin = INT16_MAX; m.vMax = INT16_MIN; }
  const AcqPq *q = &s->pq;

  if (q->ref_line >= 0 && s->ac_v[q->ref_line].validity == ACQ_VALID) {
    int32_t v = s->ac_v[q->ref_line].value;
    m.vSum += v; m.vN++;
    int16_t lo = q->events_active ? (int16_t)q->supply_min_dv : clamp16(v);
    int16_t hi = q->events_active ? (int16_t)q->supply_max_dv : clamp16(v);
    if (lo < m.vMin) m.vMin = lo;
    if (hi > m.vMax) m.vMax = hi;
  }
  if (s->ct.validity == ACQ_VALID) { m.iSum += s->ct.value; m.iN++; }
  if (q->p_valid) { m.pSum += q->p_dw; m.pN++; }
  if (q->freq_valid) { m.fSum += (int32_t)q->freq_mhz - 50000; m.fN++; }
  if (q->pf_valid) { m.pfSum += q->pf_x1000; m.pfN++; }
  if (q->v_valid) { m.vtSum += q->v_thd_x10; m.vtN++; }
  if (q->i_valid) { m.itSum += q->i_thd_x10; m.itN++; }

  if (++m.seconds < HIST_PERIOD_S) return;

  HistEntry e = {
    .v_avg_dv = avg(m.vSum, m.vN),
    .v_min_dv = m.vN ? m.vMin : HIST_MISSING,
    .v_max_dv = m.vN ? m.vMax : HIST_MISSING,
    .i_ma = avg(m.iSum, m.iN),
    .p_dw = avg(m.pSum, m.pN),
    .f_off_mhz = avg(m.fSum, m.fN),
    .pf_x1000 = avg(m.pfSum, m.pfN),
    .v_thd_x10 = avg(m.vtSum, m.vtN),
    .i_thd_x10 = avg(m.itSum, m.itN),
  };
  uint32_t nowS = (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ);
  taskENTER_CRITICAL();
  ring[head] = e;
  head = (head + 1U) % HIST_LEN;
  if (count < HIST_LEN) count++;
  newestEndS = nowS;
  taskEXIT_CRITICAL();
  Storage_SaveMinute(&e);
  m.seconds = 0U;
}

uint32_t History_Count(void)
{
  return count;
}

bool History_Get(uint32_t idx, HistEntry *out)
{
  bool ok = false;
  taskENTER_CRITICAL();
  if (idx < count) {
    uint32_t pos = (head + HIST_LEN - count + idx) % HIST_LEN;
    *out = ring[pos];
    ok = true;
  }
  taskEXIT_CRITICAL();
  return ok;
}

uint32_t History_NewestEndS(void)
{
  return newestEndS;
}
