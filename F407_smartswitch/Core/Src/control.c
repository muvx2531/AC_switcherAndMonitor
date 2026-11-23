#include "control.h"
#include "main.h"
#include "pca9536.h"
#include "debug_log.h"
#include "supervisor.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include <string.h>

extern RTC_HandleTypeDef hrtc;

/* PB3 Vin_Detect, after F101 (decision S2-3). HIGH = 24 V present, observed on the
 * bench 2026-11-23 with 24 V connected; confirm by removing 24 V (open item H2). */
#define VIN_PRESENT_LEVEL GPIO_PIN_SET
#define VIN_EXTI_LINE Vin_Detect_Pin            /* EXTI line 3 */
#define CONFIRM_LOSS_SAMPLES 3U                 /* 1 ms apart (S4-2; tune with H4) */
#define CONFIRM_RETURN_MS 200U                  /* within the 100-500 ms window */
#define POLL_MS 20U
#define READBACK_PERIOD_MS 1000U
#define UNKNOWN_RETRY_MS 500U
#define QUEUE_LEN 8U
#define OUTCOME_HISTORY 16U

#define NOTIFY_QUEUE 0x01U
#define NOTIFY_SUPPLY_EDGE 0x02U

/* pending_restore in RTC backup registers (docs/architecture/drivers/bkp-rtc.md):
 * DR1 = magic | channel mask (written last, cleared first), DR2/DR3 = durations
 * CH1|CH2 and CH3|CH4 (seconds, 16 bit each), DR4 = check value. */
#define BKP_PENDING_MAGIC 0x50520000U
#define BKP_PENDING_MAGIC_MASK 0xFFFF0000U

typedef struct { ControlRequest req; uint32_t id; } QueuedRequest;

static StaticTask_t tcb;
static uint32_t stack[512] __attribute__((section(".ccmram")));  /* never used for DMA */
static StaticQueue_t queueStorage;
static uint8_t queueBuffer[QUEUE_LEN * sizeof(QueuedRequest)];
static QueueHandle_t queue;
static TaskHandle_t task;

/* Shared with other tasks: protected by short critical sections. */
static uint32_t nextId = 1;
static CtrlOutcome outcomes[OUTCOME_HISTORY];
static ControlSnapshot published;

/* Owned by the Control task only. */
static uint8_t latch, config;
static bool stateKnown, coilSupply, releasePending, supplyCandidate;
static TickType_t supplySince, lastReadback;
static bool cycleActive[CONTROL_CHANNELS];
static TickType_t cycleDeadline[CONTROL_CHANNELS];
static uint16_t cycleDuration[CONTROL_CHANNELS];
static uint32_t cycleRequest[CONTROL_CHANNELS];
static uint8_t restoreRetry;
static CtrlSource lastSource[CONTROL_CHANNELS];
static uint32_t glitches, releases;
static const char *bootNote = "not started";

/* ---- outcomes ----------------------------------------------------------- */

static void outcomeNew(uint32_t id)
{
  CtrlOutcome *o = &outcomes[id % OUTCOME_HISTORY];
  o->request_id = id; o->state = OUT_PENDING; o->reason = NULL;
}

static void outcomeSet(uint32_t id, CtrlOutcomeState state, const char *reason)
{
  taskENTER_CRITICAL();
  CtrlOutcome *o = &outcomes[id % OUTCOME_HISTORY];
  if (o->request_id == id) { o->state = state; o->reason = reason; }
  taskEXIT_CRITICAL();
}

bool Control_GetOutcome(uint32_t request_id, CtrlOutcome *out)
{
  bool found;
  taskENTER_CRITICAL();
  const CtrlOutcome *o = &outcomes[request_id % OUTCOME_HISTORY];
  found = request_id != 0U && o->request_id == request_id;
  if (found) *out = *o;
  taskEXIT_CRITICAL();
  return found;
}

/* ---- expander access (Control is the only writer after boot) ------------ */

static bool readBack(void)
{
  PCA9536_Registers r;
  lastReadback = xTaskGetTickCount();
  if (PCA9536_ReadRegisters(&r) == I2C_BUS_OK) {
    latch = r.output & CONTROL_ALL_CHANNELS;
    config = r.config & CONTROL_ALL_CHANNELS;
    stateKnown = true;
    return true;
  }
  stateKnown = false;  /* I3: no command writes until a read succeeds */
  return false;
}

static bool writeLatch(uint8_t mask, uint8_t value)
{
  if (PCA9536_WriteMasked(mask, value) != I2C_BUS_OK) { stateKnown = false; return false; }
  return readBack() && (latch & mask) == (value & mask);
}

/* ---- pending_restore ----------------------------------------------------- */

static uint32_t bkpCheck(uint32_t mask, uint32_t d01, uint32_t d23)
{
  return (d01 * 2654435761U) ^ (d23 * 40503U) ^ mask ^ 0x5A5AA5A5U;
}

static uint8_t activeMask(void)
{
  uint8_t m = 0;
  for (unsigned int ch = 0; ch < CONTROL_CHANNELS; ch++) if (cycleActive[ch]) m |= (uint8_t)(1U << ch);
  return m;
}

static void bkpWritePending(void)
{
  uint8_t mask = activeMask();
  HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR1, 0U);  /* invalidate first */
  if (mask == 0U) return;
  uint32_t d01 = cycleDuration[0] | ((uint32_t)cycleDuration[1] << 16);
  uint32_t d23 = cycleDuration[2] | ((uint32_t)cycleDuration[3] << 16);
  HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR2, d01);
  HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR3, d23);
  HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR4, bkpCheck(mask, d01, d23));
  HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR1, BKP_PENDING_MAGIC | mask);  /* valid last */
}

static bool bkpReadPending(uint8_t *mask, uint16_t duration[CONTROL_CHANNELS])
{
  uint32_t head = HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR1);
  if ((head & BKP_PENDING_MAGIC_MASK) != BKP_PENDING_MAGIC) return false;
  uint32_t m = head & CONTROL_ALL_CHANNELS;
  uint32_t d01 = HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR2), d23 = HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR3);
  if (m == 0U || HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR4) != bkpCheck(m, d01, d23)) return false;
  *mask = (uint8_t)m;
  duration[0] = (uint16_t)d01; duration[1] = (uint16_t)(d01 >> 16);
  duration[2] = (uint16_t)d23; duration[3] = (uint16_t)(d23 >> 16);
  return true;
}

/* ---- power-cycle bookkeeping -------------------------------------------- */

/* Cancel timers on channels in mask; older requests become SUPERSEDED (S5-3). */
static void supersedeCycles(uint8_t mask, uint32_t byRequest, const char *reason)
{
  bool changed = false;
  for (unsigned int ch = 0; ch < CONTROL_CHANNELS; ch++) {
    if (!(mask & (1U << ch)) || !cycleActive[ch]) continue;
    if (cycleRequest[ch] != byRequest) outcomeSet(cycleRequest[ch], OUT_SUPERSEDED, reason);
    cycleActive[ch] = false;
    restoreRetry &= (uint8_t)~(1U << ch);
    changed = true;
  }
  if (changed) bkpWritePending();
}

static void cyclesTick(void)
{
  TickType_t now = xTaskGetTickCount();
  uint8_t due = restoreRetry;
  for (unsigned int ch = 0; ch < CONTROL_CHANNELS; ch++)
    if (cycleActive[ch] && (int32_t)(now - cycleDeadline[ch]) >= 0) { due |= (uint8_t)(1U << ch); cycleActive[ch] = false; }
  if (due == 0U) return;
  bkpWritePending();
  /* I2 (b): finishing the user's timed command. */
  if (writeLatch(due, 0U)) {
    restoreRetry = 0U;
    (void)DebugLog_Printf("CTRL: power-cycle finished, latch LOW on mask %X\r\n", due);
  } else {
    restoreRetry = due;  /* keep trying; the load stays off until this succeeds */
    (void)DebugLog_Printf("CTRL: power-cycle restore failed on mask %X, retrying\r\n", due);
  }
}

/* ---- rule (c): 24 V coil supply ----------------------------------------- */

static bool vinPresent(void) { return HAL_GPIO_ReadPin(Vin_Detect_GPIO_Port, Vin_Detect_Pin) == VIN_PRESENT_LEVEL; }

/* I2 (c): with no coil supply every coil is already off, so writing LOW changes
 * no physical state; it prevents loads going off again when 24 V returns. */
static void releaseAll(void)
{
  uint8_t wasHigh = stateKnown ? latch : CONTROL_ALL_CHANNELS;
  supersedeCycles(CONTROL_ALL_CHANNELS, 0U, "SUPPLY_LOSS");
  restoreRetry = 0U;
  HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR1, 0U);
  if (PCA9536_WriteMasked(CONTROL_ALL_CHANNELS, 0U) == I2C_BUS_OK && readBack() && latch == 0U) {
    releasePending = false;
    if (wasHigh) {
      releases++;
      (void)DebugLog_Printf("CTRL: 24 V lost, released latches %X (rule c)\r\n", wasHigh);
    }
  } else {
    stateKnown = false;
    releasePending = true;  /* the expander still has power from the battery: retry */
  }
}

static void evaluateSupply(bool edge)
{
  TickType_t now = xTaskGetTickCount();
  if (!vinPresent()) {
    supplyCandidate = false;
    bool mustCheck = edge || coilSupply || releasePending || (stateKnown && latch != 0U) || activeMask();
    if (mustCheck) {
      bool lost = true;
      for (unsigned int i = 0; i < CONFIRM_LOSS_SAMPLES; i++) {
        vTaskDelay(pdMS_TO_TICKS(1));
        if (vinPresent()) { lost = false; break; }
      }
      if (!lost) {
        glitches++;
      } else {
        if (coilSupply) (void)DebugLog_Printf("CTRL: 24 V coil supply lost\r\n");
        coilSupply = false;
        releaseAll();
      }
    }
  } else if (!coilSupply) {
    if (!supplyCandidate) { supplyCandidate = true; supplySince = now; }
    else if (now - supplySince >= pdMS_TO_TICKS(CONFIRM_RETURN_MS)) {
      supplyCandidate = false;
      coilSupply = true;
      (void)DebugLog_Printf("CTRL: 24 V coil supply present\r\n");
    }
  }
  if (edge) {
    EXTI->PR = VIN_EXTI_LINE;    /* drop edges seen while confirming */
    EXTI->IMR |= VIN_EXTI_LINE;  /* re-arm */
  }
}

void EXTI3_IRQHandler(void)
{
  if (EXTI->PR & VIN_EXTI_LINE) {
    EXTI->PR = VIN_EXTI_LINE;
    EXTI->IMR &= ~VIN_EXTI_LINE;  /* masked until the task has confirmed */
    BaseType_t wake = pdFALSE;
    if (task != NULL) xTaskNotifyFromISR(task, NOTIFY_SUPPLY_EDGE, eSetBits, &wake);
    portYIELD_FROM_ISR(wake);
  }
}

/* ---- commands ------------------------------------------------------------ */

static void process(const QueuedRequest *q)
{
  const ControlRequest *r = &q->req;
  uint8_t m = r->channel_mask & CONTROL_ALL_CHANNELS;
  if (!stateKnown) (void)readBack();
  if (!stateKnown) { outcomeSet(q->id, OUT_REJECTED, "STATE_UNKNOWN"); return; }
  if (config & m) { outcomeSet(q->id, OUT_REJECTED, "DIRECTION"); return; }
  if ((r->type == CTRL_SET_OFF || r->type == CTRL_POWER_CYCLE) && !coilSupply) {
    outcomeSet(q->id, OUT_REJECTED, "NO_COIL_SUPPLY"); return;
  }
  bool ok = true;
  TickType_t now = xTaskGetTickCount();
  switch (r->type) {
    case CTRL_SET_ON:
      supersedeCycles(m, q->id, NULL);
      ok = writeLatch(m, 0U);
      break;
    case CTRL_SET_OFF:
      supersedeCycles(m, q->id, NULL);  /* a running cycle becomes OFF hold */
      ok = writeLatch(m, m);
      break;
    case CTRL_POWER_CYCLE:
      supersedeCycles(m, q->id, NULL);  /* restart: the load does not come on in between */
      for (unsigned int ch = 0; ch < CONTROL_CHANNELS; ch++) {
        if (!(m & (1U << ch))) continue;
        cycleActive[ch] = true;
        cycleDuration[ch] = r->duration_s;
        cycleDeadline[ch] = now + pdMS_TO_TICKS((uint32_t)r->duration_s * 1000U);
        cycleRequest[ch] = q->id;
      }
      bkpWritePending();  /* before the latch changes, so a reset can finish it */
      ok = writeLatch(m, m);
      if (!ok) supersedeCycles(m, 0U, NULL);
      break;
    case CTRL_CANCEL_CYCLE: {
      uint8_t running = m & activeMask();
      supersedeCycles(running, q->id, "CANCELLED");
      ok = running ? writeLatch(running, 0U) : true;
      break;
    }
  }
  for (unsigned int ch = 0; ch < CONTROL_CHANNELS; ch++) if (m & (1U << ch)) lastSource[ch] = r->source;
  outcomeSet(q->id, ok ? OUT_APPLIED : (stateKnown ? OUT_FAILED : OUT_UNKNOWN), ok ? NULL : "I2C");
  (void)DebugLog_Printf("CTRL: request %lu type %u mask %X src %u -> %s\r\n", (unsigned long)q->id,
      (unsigned)r->type, m, (unsigned)r->source, ok ? "APPLIED" : "FAILED");
}

/* ---- snapshot ------------------------------------------------------------ */

static void publish(void)
{
  ControlSnapshot s;
  memset(&s, 0, sizeof(s));
  TickType_t now = xTaskGetTickCount();
  s.available = true;
  s.state_known = stateKnown;
  s.coil_supply = coilSupply;
  s.latch = latch;
  s.config = config;
  for (unsigned int ch = 0; ch < CONTROL_CHANNELS; ch++) {
    uint8_t bit = (uint8_t)(1U << ch);
    if (!stateKnown) s.effective[ch] = EFF_UNKNOWN;
    else if ((config & bit) || !(latch & bit) || !coilSupply) s.effective[ch] = EFF_ON;
    else s.effective[ch] = EFF_OFF;
    if (cycleActive[ch]) {
      int32_t left = (int32_t)(cycleDeadline[ch] - now);
      s.cycle_remaining_s[ch] = left > 0 ? (uint16_t)((left + configTICK_RATE_HZ - 1) / configTICK_RATE_HZ) : 0U;
    }
    s.last_source[ch] = lastSource[ch];
  }
  s.glitch_count = glitches;
  s.supply_releases = releases;
  s.boot_note = bootNote;
  taskENTER_CRITICAL();
  published = s;
  taskEXIT_CRITICAL();
}

void Control_GetSnapshot(ControlSnapshot *out)
{
  taskENTER_CRITICAL();
  *out = published;
  taskEXIT_CRITICAL();
}

/* ---- boot ---------------------------------------------------------------- */

static void bootReconcile(void)
{
  for (unsigned int i = 0; i < 3U && !readBack(); i++) vTaskDelay(pdMS_TO_TICKS(50));
  bool present = true;
  for (unsigned int i = 0; i < CONFIRM_LOSS_SAMPLES && present; i++) {
    present = vinPresent();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  coilSupply = present;

  uint8_t mask;
  uint16_t duration[CONTROL_CHANNELS];
  if (!bkpReadPending(&mask, duration)) {
    bootNote = "no pending power-cycle";
  } else if (!stateKnown) {
    bootNote = "pending power-cycle kept: expander state unknown";  /* re-checked next boot */
  } else {
    TickType_t now = xTaskGetTickCount();
    uint8_t resumed = 0;
    for (unsigned int ch = 0; ch < CONTROL_CHANNELS; ch++) {
      uint8_t bit = (uint8_t)(1U << ch);
      /* Resume only where the coil is still commanded on; a LOW latch means the
       * expander lost power or the cycle already ended (I4: nothing restored). */
      if (!(mask & bit) || (config & bit) || !(latch & bit) || duration[ch] == 0U) continue;
      cycleActive[ch] = true;
      cycleDuration[ch] = duration[ch];
      /* Full duration from boot: the load stays off a bit longer, never shorter. */
      cycleDeadline[ch] = now + pdMS_TO_TICKS((uint32_t)duration[ch] * 1000U);
      cycleRequest[ch] = 0U;
      resumed |= bit;
    }
    bootNote = resumed ? "pending power-cycle resumed after reset" : "pending power-cycle dropped (latch low)";
    bkpWritePending();
    (void)DebugLog_Printf("CTRL: pending power-cycle mask %X, resumed %X\r\n", mask, resumed);
  }
  if (!coilSupply) releaseAll();  /* rule (c) also applies at boot */
  (void)DebugLog_Printf("CTRL: start latch=%X config=%X known=%u coil_supply=%u (%s)\r\n",
      latch, config, stateKnown, coilSupply, bootNote);
}

static void controlTask(void *argument)
{
  (void)argument;
  bootReconcile();
  GPIO_InitTypeDef gpio = {0};
  gpio.Pin = Vin_Detect_Pin;
  gpio.Mode = GPIO_MODE_IT_RISING_FALLING;
  gpio.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(Vin_Detect_GPIO_Port, &gpio);
  HAL_NVIC_SetPriority(EXTI3_IRQn, 5, 0);  /* FreeRTOS-compatible */
  HAL_NVIC_EnableIRQ(EXTI3_IRQn);
  publish();

  for (;;) {
    uint32_t bits = 0;
    (void)xTaskNotifyWait(0U, 0xFFFFFFFFU, &bits, pdMS_TO_TICKS(POLL_MS));
    evaluateSupply((bits & NOTIFY_SUPPLY_EDGE) != 0U);  /* rule (c) before queued commands */
    QueuedRequest q;
    while (xQueueReceive(queue, &q, 0) == pdTRUE) process(&q);
    cyclesTick();
    TickType_t now = xTaskGetTickCount();
    if (now - lastReadback >= pdMS_TO_TICKS(stateKnown ? READBACK_PERIOD_MS : UNKNOWN_RETRY_MS)) (void)readBack();
    Supervisor_ControlHeartbeat();
    publish();
  }
}

bool Control_Start(void)
{
  if (task != NULL) return true;
  HAL_PWR_EnableBkUpAccess();
  queue = xQueueCreateStatic(QUEUE_LEN, sizeof(QueuedRequest), queueBuffer, &queueStorage);
  if (queue == NULL) return false;
  task = xTaskCreateStatic(controlTask, "control", sizeof(stack) / sizeof(stack[0]), NULL,
      osPriorityAboveNormal, stack, &tcb);
  return task != NULL;
}

/* ---- submit -------------------------------------------------------------- */

CtrlSubmitResult Control_Submit(const ControlRequest *req, uint32_t *out_request_id)
{
  if (task == NULL || req == NULL) return CTRL_SUBMIT_NOT_READY;
  if (__get_IPSR() != 0U || req->type > CTRL_CANCEL_CYCLE) return CTRL_SUBMIT_REJECTED_INVALID;
  if (req->channel_mask == 0U || (req->channel_mask & ~CONTROL_ALL_CHANNELS) != 0U) return CTRL_SUBMIT_REJECTED_INVALID;
  QueuedRequest q = { .req = *req, .id = 0U };
  if (req->type == CTRL_POWER_CYCLE) {
    if (q.req.duration_s == 0U) q.req.duration_s = CONTROL_DEFAULT_CYCLE_S;
    if (q.req.duration_s > CONTROL_MAX_CYCLE_S) return CTRL_SUBMIT_REJECTED_INVALID;
  }
  ControlSnapshot s;
  Control_GetSnapshot(&s);
  if (!s.state_known) return CTRL_SUBMIT_REJECTED_STATE_UNKNOWN;
  if ((req->type == CTRL_SET_OFF || req->type == CTRL_POWER_CYCLE) && !s.coil_supply)
    return CTRL_SUBMIT_REJECTED_NO_COIL_SUPPLY;  /* S5-5 */
  taskENTER_CRITICAL();
  q.id = nextId++;
  if (nextId == 0U) nextId = 1U;
  outcomeNew(q.id);
  taskEXIT_CRITICAL();
  if (xQueueSend(queue, &q, 0) != pdTRUE) { outcomeSet(q.id, OUT_REJECTED, "BUSY"); return CTRL_SUBMIT_BUSY; }
  (void)xTaskNotify(task, NOTIFY_QUEUE, eSetBits);
  if (out_request_id != NULL) *out_request_id = q.id;
  return CTRL_SUBMIT_ACCEPTED;
}

const char *Control_OutcomeName(CtrlOutcomeState state)
{
  static const char *const names[] = {"PENDING", "APPLIED", "FAILED", "SUPERSEDED", "UNKNOWN", "REJECTED"};
  return (unsigned)state < sizeof(names) / sizeof(names[0]) ? names[state] : "UNKNOWN";
}

const char *Control_SubmitResultName(CtrlSubmitResult result)
{
  switch (result) {
    case CTRL_SUBMIT_ACCEPTED: return "ACCEPTED";
    case CTRL_SUBMIT_BUSY: return "BUSY";
    case CTRL_SUBMIT_REJECTED_INVALID: return "INVALID";
    case CTRL_SUBMIT_REJECTED_NO_COIL_SUPPLY: return "NO_COIL_SUPPLY";
    case CTRL_SUBMIT_REJECTED_STATE_UNKNOWN: return "STATE_UNKNOWN";
    default: return "NOT_READY";
  }
}
