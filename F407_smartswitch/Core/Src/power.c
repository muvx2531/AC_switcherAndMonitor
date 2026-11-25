#include "power.h"
#include "acquisition.h"
#include "boot.h"
#include "control.h"
#include "debug_log.h"
#include "eth_probe.h"
#include "supervisor.h"
#include "storage.h"
#include "power_quality.h"
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os2.h"

#define POLL_MS 100U
#define RESUME_RETRY_MS 5000U

static TaskHandle_t task;
static StaticTask_t tcb;
static StackType_t stack[512] __attribute__((section(".ccmram")));  /* holds an AcqSnapshot copy */

static PowerSnapshot snap;
static TickType_t modeSince;

static void setAmcRail(bool on)
{
  /* With no AMC3330 fitted there is nothing to power. */
  HAL_GPIO_WritePin(En_3v3_AMC_GPIO_Port, En_3v3_AMC_Pin, (on && Acquisition_AmcFitted()) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static bool amcRailOn(void)
{
  return (En_3v3_AMC_GPIO_Port->ODR & En_3v3_AMC_Pin) != 0U;
}

static void publish(PowerMode mode, bool ethRail, const char *reason, bool transition, bool resumeFailed)
{
  taskENTER_CRITICAL();
  snap.mode = mode;
  snap.eth_rail = ethRail;
  snap.amc_rail = amcRailOn();
  if (reason != NULL) snap.last_reason = reason;
  if (transition) snap.transitions++;
  if (resumeFailed) snap.eth_resume_failures++;
  taskEXIT_CRITICAL();
}

static void enter(PowerMode *mode, PowerMode next, const char *reason, bool *ethRail)
{
  PowerMode prev = *mode;
  *mode = next;
  modeSince = xTaskGetTickCount();
  if (next == POWER_MAINS) {
    setAmcRail(true);
    /* ETH resume is attempted by the loop (bounded, retried). */
  } else if (prev == POWER_MAINS) {
    setAmcRail(false);
    if (EthProbe_Suspend()) *ethRail = false;
    Storage_CheckpointEnergy(Acquisition_EnergyUwh());   /* on battery: save the counter now */
  }
  Storage_LogEvent(ST_EV_POWER_MODE, (int32_t)prev, (int32_t)next, 0, 0);
  (void)DebugLog_Printf("PWR: %s -> %s (%s)\r\n", Power_ModeName(prev), Power_ModeName(next), reason);
  publish(next, *ethRail, reason, true, false);
}

static void powerTask(void *arg)
{
  (void)arg;
  PowerMode mode = POWER_MAINS;
  EthInfo ei;
  EthProbe_GetInfo(&ei);
  bool ethRail = ei.powered;
  TickType_t supplyEdge = xTaskGetTickCount(), lowVbatSince = 0, lastResumeTry = 0;
  bool lastSupply = true, lowVbat = false;
  modeSince = xTaskGetTickCount();
  setAmcRail(true);
  publish(mode, ethRail, "boot", false, false);

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    Supervisor_TaskHeartbeat(SUP_TASK_POWER);
    TickType_t now = xTaskGetTickCount();

    ControlSnapshot c;
    Control_GetSnapshot(&c);
    bool supply = c.available ? c.coil_supply : true;   /* Control confirms PB3 (S4-2) */
    if (supply != lastSupply) { lastSupply = supply; supplyEdge = now; }
    TickType_t stable = now - supplyEdge;

    AcqSnapshot a;
    Acquisition_GetSnapshot(&a);
    bool vbatLow = a.vbat.validity == ACQ_VALID && a.vbat.value < POWER_LOW_VBAT_MV;
    if (vbatLow && !lowVbat) lowVbatSince = now;
    lowVbat = vbatLow;

    switch (mode) {
      case POWER_MAINS:
        if (!supply && stable >= pdMS_TO_TICKS(POWER_BACKUP_DELAY_MS))
          enter(&mode, POWER_BACKUP, "24 V absent", &ethRail);
        else if (supply && !ethRail && !Boot_SafeMode() &&
                 (lastResumeTry == 0U || now - lastResumeTry >= pdMS_TO_TICKS(RESUME_RETRY_MS))) {
          lastResumeTry = now;
          ei.available = false;
          EthProbe_GetInfo(&ei);
          if (ei.available) {
            bool ok = EthProbe_Resume();
            if (ok) { ethRail = true; lastResumeTry = 0U; }
            publish(mode, ethRail, NULL, false, !ok);
          }
        }
        break;
      case POWER_BACKUP:
      case POWER_LOW_POWER:
        if (supply && stable >= pdMS_TO_TICKS(POWER_RETURN_DELAY_MS)) {
          lastResumeTry = 0U;
          enter(&mode, POWER_MAINS, "24 V present", &ethRail);
        } else if (mode == POWER_BACKUP) {
          if (lowVbat && now - lowVbatSince >= pdMS_TO_TICKS(POWER_LOW_VBAT_HOLD_MS))
            enter(&mode, POWER_LOW_POWER, "VBAT low", &ethRail);
          else if (now - modeSince >= pdMS_TO_TICKS(POWER_LOW_AFTER_MS))
            enter(&mode, POWER_LOW_POWER, "8 h on battery", &ethRail);
        }
        break;
    }
    taskENTER_CRITICAL();
    snap.mode_age_ms = (uint32_t)((now - modeSince) * portTICK_PERIOD_MS);
    snap.amc_rail = amcRailOn();
    taskEXIT_CRITICAL();
  }
}

bool Power_Start(void)
{
  if (task != NULL) return true;
  snap.last_reason = "boot";
  /* Same level as Acquisition, above the default task; Supervisor stays lower. */
  task = xTaskCreateStatic(powerTask, "power", sizeof(stack) / sizeof(stack[0]), NULL,
      osPriorityNormal1, stack, &tcb);
  return task != NULL;
}

void Power_GetSnapshot(PowerSnapshot *out)
{
  if (out == NULL) return;
  taskENTER_CRITICAL();
  *out = snap;
  taskEXIT_CRITICAL();
}

const char *Power_ModeName(PowerMode m)
{
  switch (m) {
    case POWER_MAINS: return "MAINS";
    case POWER_BACKUP: return "BACKUP";
    default: return "LOW_POWER";
  }
}
