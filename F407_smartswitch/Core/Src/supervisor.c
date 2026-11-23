#include "supervisor.h"
#include "boot.h"
#include "debug_log.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"

#define KICK_PERIOD_MS 200U        /* STWD100 timeout 1.12 s */
#define BOOT_DEADLINE_MS 15000U    /* whole boot incl. network init */
#define HEARTBEAT_TIMEOUT_MS 2000U

typedef enum { PHASE_BOOT, PHASE_RUN, PHASE_BOOT_TIMEOUT, PHASE_HEARTBEAT_LOST } Phase;

static volatile Phase phase = PHASE_BOOT;
static volatile TickType_t lastHeartbeat;
static volatile TickType_t lastControlHeartbeat;
static volatile bool controlRegistered;

/* Static allocation; the stack lives in CCM (never used for DMA). */
static StaticTask_t tcb;
static uint32_t stack[256] __attribute__((section(".ccmram")));

static void supervisorTask(void *argument)
{
  (void)argument;
  const TickType_t start = xTaskGetTickCount();
  bool reported = false;
  for (;;) {
    TickType_t now = xTaskGetTickCount();
    bool kick = false;
    if (phase == PHASE_BOOT) {
      if (now - start < pdMS_TO_TICKS(BOOT_DEADLINE_MS)) kick = true;
      else phase = PHASE_BOOT_TIMEOUT;
    } else if (phase == PHASE_RUN) {
      bool defaultOk = now - lastHeartbeat < pdMS_TO_TICKS(HEARTBEAT_TIMEOUT_MS);
      bool controlOk = !controlRegistered || now - lastControlHeartbeat < pdMS_TO_TICKS(HEARTBEAT_TIMEOUT_MS);
      if (defaultOk && controlOk) kick = true;
      else phase = PHASE_HEARTBEAT_LOST;
    }
    if (kick) {
      Boot_WdtKick();
    } else if (!reported) {
      /* With jumper DIS_WDT fitted the board keeps running; otherwise it resets. */
      reported = true;
      (void)DebugLog_Printf("SUP: stopped kicking watchdog (%s)\r\n", Supervisor_Phase());
    }
    vTaskDelay(pdMS_TO_TICKS(KICK_PERIOD_MS));
  }
}

bool Supervisor_Start(void)
{
  const osThreadAttr_t attr = {
    .name = "supervisor",
    .cb_mem = &tcb, .cb_size = sizeof(tcb),
    .stack_mem = stack, .stack_size = sizeof(stack),
    .priority = osPriorityBelowNormal,
  };
  return osThreadNew(supervisorTask, NULL, &attr) != NULL;
}

void Supervisor_BootComplete(void)
{
  lastHeartbeat = xTaskGetTickCount();
  if (phase == PHASE_BOOT) phase = PHASE_RUN;
}

void Supervisor_Heartbeat(void)
{
  lastHeartbeat = xTaskGetTickCount();
}

void Supervisor_ControlHeartbeat(void)
{
  lastControlHeartbeat = xTaskGetTickCount();
  controlRegistered = true;
}

const char *Supervisor_Phase(void)
{
  switch (phase) {
    case PHASE_BOOT: return "boot";
    case PHASE_RUN: return "run";
    case PHASE_BOOT_TIMEOUT: return "boot_timeout";
    default: return "heartbeat_lost";
  }
}
