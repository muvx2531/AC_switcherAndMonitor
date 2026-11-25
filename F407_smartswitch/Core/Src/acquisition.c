#include "acquisition.h"
#include "debug_log.h"
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os2.h"
#include <math.h>
#include <string.h>

/* Register-level driver: MX_ADC2_Init() still runs (it configures ADC2 GPIO as analog
 * through HAL_ADC_MspInit), but this file reprograms ADC1/ADC2 from scratch and never
 * uses the HAL ADC state machine. ADC2's own DMA (DMA2 Stream3) is left disabled: in
 * dual mode the common data register is read by the ADC1 request (DMA2 Stream0 ch0). */

#define SAMPLE_HZ        6400U
#define RANKS            5U
#define HALF_SAMPLES     64U                       /* 10 ms */
#define BUF_WORDS        (2U * HALF_SAMPLES * RANKS)
#define HALVES_PER_WINDOW 100U                     /* 1 s */
#define STALL_MS         50U
#define STALE_MS         2500U
#define RESTART_GAP_MS   1000U
#define RAIL_SETTLE_MS   1000U
#define ADC_LOW_RAIL     8U
#define ADC_HIGH_RAIL    4087U
#define TIM3_ARR         (84000000U / SAMPLE_HZ - 1U)   /* APB1 timer clock 84 MHz */
#define ADC_EXTSEL_T3_TRGO 8U

/* Nominal scaling (VREF+ = 3.000 V). cal_version 0 = not calibrated. */
#define VOLT_PER_COUNT   (3.0f / 4096.0f)
#define AC_SCALE_X10     (VOLT_PER_COUNT * 200.1f * 10.0f)  /* (P-N) volts x 200.1 = line V */
#define CT_MA_PER_COUNT  7.33f
#define FAILSAFE_COUNTS  (-2730)                             /* (P-N) < -2.0 V: AMC fail-safe */

/* DMA target: plain SRAM (.bss), never CCM. One word = ADC2 << 16 | ADC1. */
static volatile uint32_t buf[BUF_WORDS] __attribute__((aligned(4)));

static TaskHandle_t task;
static StaticTask_t tcb;
static StackType_t stack[384] __attribute__((section(".ccmram")));

static volatile uint32_t halvesDone;
static volatile bool dmaError;

typedef struct {
  int32_t sum[ACQ_AC_CHANNELS];
  uint64_t sumsq[ACQ_AC_CHANNELS];
  bool rail[ACQ_AC_CHANNELS];      /* any P or N sample at an ADC rail */
  uint32_t ctSum;
  uint64_t ctSumsq;
  bool ctRail;
  uint32_t vbatSum;
  uint32_t count;
  uint32_t halves;
} Window;

static Window win;
static AcqSnapshot snap;           /* published copy, guarded by a critical section */
static TickType_t snapTick[ACQ_AC_CHANNELS + 2U];   /* compute time per signal: ac[0..3], ct, vbat */

/* ---- hardware ------------------------------------------------------------ */

static void hwStop(void)
{
  TIM3->CR1 &= ~TIM_CR1_CEN;
  DMA2_Stream0->CR &= ~DMA_SxCR_EN;
  for (unsigned int i = 0; i < 10000U && (DMA2_Stream0->CR & DMA_SxCR_EN); i++) { }
  ADC1->CR2 &= ~ADC_CR2_ADON;
  ADC2->CR2 &= ~ADC_CR2_ADON;
  ADC1->SR = 0U;
  ADC2->SR = 0U;
  DMA2->LIFCR = DMA_LIFCR_CFEIF0 | DMA_LIFCR_CDMEIF0 | DMA_LIFCR_CTEIF0 | DMA_LIFCR_CHTIF0 | DMA_LIFCR_CTCIF0;
}

static void hwStart(void)
{
  __HAL_RCC_ADC1_CLK_ENABLE();
  __HAL_RCC_ADC2_CLK_ENABLE();
  __HAL_RCC_TIM3_CLK_ENABLE();
  __HAL_RCC_DMA2_CLK_ENABLE();
  hwStop();

  /* Common: ADCCLK = PCLK2/4 = 21 MHz, DMA mode 2 (pairs packed in one word),
   * continuous DMA requests, dual regular simultaneous. */
  ADC123_COMMON->CCR = ADC_CCR_ADCPRE_0 | ADC_CCR_DMA_1 | ADC_CCR_DDS | ADC_CCR_MULTI_2 | ADC_CCR_MULTI_1;

  /* 480-cycle sampling on every channel: 5 x 492 / 21 MHz = 117 us < 156 us period.
   * Long sampling suits the ~37 kOhm VBAT divider and the CT/AMC RC filters. */
  ADC1->SMPR1 = ADC2->SMPR1 = 0x07FFFFFFU;
  ADC1->SMPR2 = ADC2->SMPR2 = 0x3FFFFFFFU;
  ADC1->CR1 = ADC2->CR1 = ADC_CR1_SCAN;
  ADC1->SQR1 = ADC2->SQR1 = (RANKS - 1U) << ADC_SQR1_L_Pos;
  ADC1->SQR2 = ADC2->SQR2 = 0U;
  ADC1->SQR3 = (10U << 0) | (0U << 5) | (4U << 10) | (8U << 15) | (6U << 20);
  ADC2->SQR3 = (12U << 0) | (3U << 5) | (5U << 10) | (9U << 15) | (13U << 20);
  ADC1->CR2 = ADC_CR2_EXTEN_0 | (ADC_EXTSEL_T3_TRGO << ADC_CR2_EXTSEL_Pos);  /* master: TIM3 TRGO rising */
  ADC2->CR2 = 0U;                                                          /* slave follows master */
  ADC1->CR2 |= ADC_CR2_ADON;
  ADC2->CR2 |= ADC_CR2_ADON;
  for (volatile unsigned int i = 0; i < 2000U; i++) { }                   /* tSTAB > 3 us */

  halvesDone = 0U;
  dmaError = false;
  DMA2_Stream0->CR = 0U;
  DMA2_Stream0->PAR = (uint32_t)&ADC123_COMMON->CDR;
  DMA2_Stream0->M0AR = (uint32_t)buf;
  DMA2_Stream0->NDTR = BUF_WORDS;
  DMA2_Stream0->FCR = 0U;   /* direct mode */
  DMA2_Stream0->CR = (0U << DMA_SxCR_CHSEL_Pos) | DMA_SxCR_PL_1 | DMA_SxCR_MSIZE_1 | DMA_SxCR_PSIZE_1 |
      DMA_SxCR_MINC | DMA_SxCR_CIRC | DMA_SxCR_HTIE | DMA_SxCR_TCIE | DMA_SxCR_TEIE | DMA_SxCR_DMEIE;
  HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, 6, 0);   /* numerically >= configMAX_SYSCALL (5) */
  HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);
  DMA2_Stream0->CR |= DMA_SxCR_EN;

  TIM3->CR1 = 0U;
  TIM3->PSC = 0U;
  TIM3->ARR = TIM3_ARR;
  TIM3->CR2 = TIM_CR2_MMS_1;     /* update event -> TRGO */
  TIM3->EGR = TIM_EGR_UG;
  TIM3->SR = 0U;
  TIM3->CR1 = TIM_CR1_CEN;
}

void DMA2_Stream0_IRQHandler(void)
{
  uint32_t isr = DMA2->LISR;
  DMA2->LIFCR = isr & (DMA_LISR_FEIF0 | DMA_LISR_DMEIF0 | DMA_LISR_TEIF0 | DMA_LISR_HTIF0 | DMA_LISR_TCIF0);
  if (isr & (DMA_LISR_TEIF0 | DMA_LISR_DMEIF0)) dmaError = true;
  if (isr & DMA_LISR_HTIF0) halvesDone++;
  if (isr & DMA_LISR_TCIF0) halvesDone++;
  BaseType_t woken = pdFALSE;
  if (task != NULL) vTaskNotifyGiveFromISR(task, &woken);
  portYIELD_FROM_ISR(woken);
}

/* ---- processing ---------------------------------------------------------- */

static void accumulate(unsigned int half)
{
  const volatile uint32_t *p = &buf[half * HALF_SAMPLES * RANKS];
  for (unsigned int s = 0; s < HALF_SAMPLES; s++, p += RANKS) {
    for (unsigned int ch = 0; ch < ACQ_AC_CHANNELS; ch++) {
      uint32_t w = p[ch];
      uint16_t pv = (uint16_t)(w & 0xFFFFU), nv = (uint16_t)(w >> 16);
      int32_t d = (int32_t)pv - (int32_t)nv;
      win.sum[ch] += d;
      win.sumsq[ch] += (uint64_t)((int64_t)d * d);
      if (pv <= ADC_LOW_RAIL || pv >= ADC_HIGH_RAIL || nv <= ADC_LOW_RAIL || nv >= ADC_HIGH_RAIL) win.rail[ch] = true;
    }
    uint32_t w = p[4];
    uint16_t ct = (uint16_t)(w & 0xFFFFU), vb = (uint16_t)(w >> 16);
    win.ctSum += ct;
    win.ctSumsq += (uint64_t)ct * ct;
    if (ct <= ADC_LOW_RAIL || ct >= ADC_HIGH_RAIL) win.ctRail = true;
    win.vbatSum += vb;
  }
  win.count += HALF_SAMPLES;
  win.halves++;
}

static float rmsCounts(int64_t sum, uint64_t sumsq, uint32_t n)
{
  float mean = (float)sum / (float)n;
  float var = (float)sumsq / (float)n - mean * mean;
  return var > 0.0f ? sqrtf(var) : 0.0f;
}

static bool readDiag(unsigned int ch)
{
  static GPIO_TypeDef *const port[ACQ_AC_CHANNELS] = { DIAG_S1_GPIO_Port, DIAG_S2_GPIO_Port, DIAG_S3_GPIO_Port, DIAG_S4_GPIO_Port };
  static const uint16_t pin[ACQ_AC_CHANNELS] = { DIAG_S1_Pin, DIAG_S2_Pin, DIAG_S3_Pin, DIAG_S4_Pin };
  return HAL_GPIO_ReadPin(port[ch], pin[ch]) == GPIO_PIN_SET;
}

static void finishWindow(void)
{
  static TickType_t railOnSince;
  static bool railWasOn;
  TickType_t now = xTaskGetTickCount();
  uint32_t n = win.count;
  AcqSnapshot s;
  taskENTER_CRITICAL();
  s = snap;
  taskEXIT_CRITICAL();

  bool railOn = (En_3v3_AMC_GPIO_Port->ODR & En_3v3_AMC_Pin) != 0U;
  if (railOn && !railWasOn) railOnSince = now;
  railWasOn = railOn;

  for (unsigned int ch = 0; ch < ACQ_AC_CHANNELS; ch++) {
    AcqSignal *a = &s.ac_v[ch];
    float rms = rmsCounts(win.sum[ch], win.sumsq[ch], n);
    s.diag[ch] = readDiag(ch) ? 1U : 0U;
    a->raw = (uint16_t)(rms + 0.5f);
    a->value = (int32_t)(rms * AC_SCALE_X10 + 0.5f);
    if (!ACQ_AMC_FITTED) a->validity = ACQ_NOT_FITTED;
    else if (!railOn) a->validity = ACQ_RAIL_OFF;
    else if (now - railOnSince < pdMS_TO_TICKS(RAIL_SETTLE_MS)) a->validity = ACQ_NO_DATA;
    else if (!s.diag[ch] || win.sum[ch] / (int32_t)n < FAILSAFE_COUNTS) a->validity = ACQ_DIAG_FAIL;
    else if (win.rail[ch]) a->validity = ACQ_OUT_OF_RANGE;
    else a->validity = ACQ_VALID;
    snapTick[ch] = now;
  }

  float ctRms = rmsCounts((int64_t)win.ctSum, win.ctSumsq, n);
  s.ct.raw = (uint16_t)((win.ctSum + n / 2U) / n);
  s.ct.value = (int32_t)(ctRms * CT_MA_PER_COUNT + 0.5f);
  s.ct.validity = win.ctRail ? ACQ_OUT_OF_RANGE : ACQ_VALID;
  snapTick[ACQ_AC_CHANNELS] = now;

  /* Same formula as the bring-up estimate: 3000 mV x (49.9k + 49.9k + 56k) / 56k / 4095. */
  s.vbat.raw = (uint16_t)((win.vbatSum + n / 2U) / n);
  s.vbat.value = (int32_t)(((uint64_t)win.vbatSum * 3000U * 1558U + (uint64_t)n * 4095U * 560U / 2U) /
      ((uint64_t)n * 4095U * 560U));
  s.vbat.validity = (s.vbat.raw <= ADC_LOW_RAIL || s.vbat.raw >= ADC_HIGH_RAIL) ? ACQ_OUT_OF_RANGE : ACQ_VALID;
  snapTick[ACQ_AC_CHANNELS + 1U] = now;

  s.cal_version = 0U;
  taskENTER_CRITICAL();
  snap.ac_v[0] = s.ac_v[0]; snap.ac_v[1] = s.ac_v[1]; snap.ac_v[2] = s.ac_v[2]; snap.ac_v[3] = s.ac_v[3];
  snap.ct = s.ct;
  snap.vbat = s.vbat;
  memcpy(snap.diag, s.diag, sizeof(snap.diag));
  snap.cal_version = s.cal_version;
  taskEXIT_CRITICAL();
  memset(&win, 0, sizeof(win));
}

static void setRunning(bool running, bool restarted, uint32_t missed, bool cycleDone)
{
  taskENTER_CRITICAL();
  snap.running = running;
  if (restarted) snap.restarts++;
  snap.missed_halves += missed;
  if (cycleDone) snap.cycles++;
  taskEXIT_CRITICAL();
}

static void acquisitionTask(void *arg)
{
  (void)arg;
  uint32_t consumed = 0U;
  TickType_t lastRestart = xTaskGetTickCount();
  hwStart();
  setRunning(true, false, 0U, false);

  for (;;) {
    bool got = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(STALL_MS)) != 0U;
    bool adcOverrun = (ADC123_COMMON->CSR & (ADC_CSR_OVR1 | ADC_CSR_OVR2)) != 0U;
    if (!got || dmaError || adcOverrun) {
      setRunning(false, false, 0U, false);
      TickType_t now = xTaskGetTickCount();
      if (now - lastRestart < pdMS_TO_TICKS(RESTART_GAP_MS)) continue;   /* bounded restarts */
      (void)DebugLog_Printf("ACQ: restart (%s)\r\n", !got ? "stall" : dmaError ? "DMA error" : "ADC overrun");
      lastRestart = now;
      memset(&win, 0, sizeof(win));
      consumed = 0U;
      hwStart();
      setRunning(true, true, 0U, false);
      continue;
    }
    uint32_t done = halvesDone;
    uint32_t pending = done - consumed;
    if (pending == 0U) continue;
    uint32_t missed = pending - 1U;     /* older halves were already overwritten */
    consumed = done;
    accumulate((done - 1U) & 1U);       /* HT completes half 0, TC half 1 */
    bool cycleDone = (win.halves % 2U) == 0U;
    setRunning(true, false, missed, cycleDone);
    if (win.halves >= HALVES_PER_WINDOW) finishWindow();
  }
}

/* ---- API ----------------------------------------------------------------- */

bool Acquisition_Start(void)
{
  if (task != NULL) return true;
#if ACQ_AMC_FITTED
  /* TEMPORARY until the Power service (Phase 4) owns the rails: turn on +3.3V_AMC (PC10)
   * so the AMC3330 channels can be measured. Power will request/release it later. */
  HAL_GPIO_WritePin(En_3v3_AMC_GPIO_Port, En_3v3_AMC_Pin, GPIO_PIN_SET);
#endif
  memset(&snap, 0, sizeof(snap));
  for (unsigned int ch = 0; ch < ACQ_AC_CHANNELS; ch++)
    snap.ac_v[ch].validity = ACQ_AMC_FITTED ? ACQ_NO_DATA : ACQ_NOT_FITTED;
  /* Below Control (AboveNormal), above the default task and HTTP connections. */
  task = xTaskCreateStatic(acquisitionTask, "acq", sizeof(stack) / sizeof(stack[0]), NULL,
      osPriorityNormal1, stack, &tcb);
  if (task != NULL)
    (void)DebugLog_Printf("ACQ: dual ADC %u Hz, AMC3330 %s\r\n", SAMPLE_HZ, ACQ_AMC_FITTED ? "fitted" : "not fitted");
  return task != NULL;
}

static void age(AcqSignal *s, TickType_t at, TickType_t now)
{
  if (s->validity == ACQ_NO_DATA || s->validity == ACQ_NOT_FITTED) { s->age_ms = 0U; return; }
  s->age_ms = (uint32_t)((now - at) * portTICK_PERIOD_MS);
  if (s->age_ms > STALE_MS) s->validity = ACQ_STALE;
}

void Acquisition_GetSnapshot(AcqSnapshot *out)
{
  if (out == NULL) return;
  TickType_t t[ACQ_AC_CHANNELS + 2U];
  taskENTER_CRITICAL();
  *out = snap;
  memcpy(t, snapTick, sizeof(t));
  taskEXIT_CRITICAL();
  TickType_t now = xTaskGetTickCount();
  for (unsigned int ch = 0; ch < ACQ_AC_CHANNELS; ch++) age(&out->ac_v[ch], t[ch], now);
  age(&out->ct, t[ACQ_AC_CHANNELS], now);
  age(&out->vbat, t[ACQ_AC_CHANNELS + 1U], now);
}

const char *Acquisition_ValidityName(AcqValidity v)
{
  switch (v) {
    case ACQ_VALID: return "VALID";
    case ACQ_STALE: return "STALE";
    case ACQ_OUT_OF_RANGE: return "OUT_OF_RANGE";
    case ACQ_DIAG_FAIL: return "DIAG_FAIL";
    case ACQ_RAIL_OFF: return "RAIL_OFF";
    case ACQ_NOT_FITTED: return "NOT_FITTED";
    default: return "NO_DATA";
  }
}
