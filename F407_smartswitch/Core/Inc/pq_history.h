#ifndef PQ_HISTORY_H
#define PQ_HISTORY_H
#include "acquisition.h"
#include <stdbool.h>
#include <stdint.h>

/* 24 h power-quality history: one entry per minute, 1440 entries in CCM (RAM only,
 * lost on reset until the Storage phase). Written by the Acquisition task once per
 * second (History_AddSecond); read by the web server (History_Get). docs/ac-quality.md */

#define HIST_LEN       1440U
#define HIST_PERIOD_S  60U
#define HIST_MISSING   INT16_MIN

typedef struct {
  int16_t v_avg_dv, v_min_dv, v_max_dv;   /* supply V x10: mean of 1 s values, min/max of half-cycle RMS */
  int16_t i_ma;                           /* mean current, mA */
  int16_t p_dw;                           /* mean active power, 0.1 W */
  int16_t f_off_mhz;                      /* mean frequency - 50 Hz, mHz */
  int16_t pf_x1000;                       /* mean true PF */
  int16_t v_thd_x10, i_thd_x10;           /* mean THD, % x10 */
} HistEntry;                              /* 18 bytes, little endian on the wire */

void History_AddSecond(const AcqSnapshot *s);
uint32_t History_Count(void);
/* idx 0 = oldest. Returns false when idx >= count. */
bool History_Get(uint32_t idx, HistEntry *out);
/* Uptime (s) at the end of the newest entry. */
uint32_t History_NewestEndS(void);

#endif
