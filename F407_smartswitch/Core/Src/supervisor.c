#include "supervisor.h"
#include "boot.h"
#include "debug_log.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"

#define KICK_PERIOD_MS 200U        /* STWD100 timeout 1.12 s */
#define BOOT_DEADLINE_MS 15000U    /* whole boot incl. network init */
#define HEARTBEAT_TIMEOUT_MS 2000U
#define STABLE_UPTIME_MS (BOOT_SAFE_MODE_WINDOW_S * 1000U)

typedef enum { PHASE_BOOT, PHASE_RUN, PHASE_BOOT_TIMEOUT, PHASE_HEARTBEAT_LOST } Phase;

static volatile Phase phase = PHASE_BOOT;
static volatile TickType_t lastBeat[SUP_TASK_COUNT];
static volatile bool registered[SUP_TASK_COUNT];
static volatile int lostTask = -1;

static const char *const taskNames[SUP_TASK_COUNT] = { "defaultTask", "control", "acq", "power", "storage" };

/* Static allocation; the stack lives in CCM (never used for DMA). */
static StaticTask_t tcb;
static uint32_t stack[256] __attribute__((section(".ccmram")));

static void supervisorTask(void *argument)
{
  (void)argument;
  const TickType_t start = xTaskGetTickCount();
  bool reported = false, counterCleared = false;
  for (;;) {
    TickType_t now = xTaskGetTickCount();
    bool kick = false;
    if (phase == PHASE_BOOT) {
      if (now - start < pdMS_TO_TICKS(BOOT_DEADLINE_MS)) kick = true;
      else phase = PHASE_BOOT_TIMEOUT;
    } else if (phase == PHASE_RUN) {
      kick = true;
      for (int t = 0; t < (int)SUP_TASK_COUNT; t++) {
        if (registered[t] && now - lastBeat[t] >= pdMS_TO_TICKS(HEARTBEAT_TIMEOUT_MS)) {
          kick = false; lostTask = t; phase = PHASE_HEARTBEAT_LOST; break;
        }
      }
      if (kick && !counterCleared && now - start >= pdMS_TO_TICKS(STABLE_UPTIME_MS)) {
        Boot_ClearResetCount();
        counterCleared = true;
        (void)DebugLog_Printf("SUP: stable for %u min, reset counter cleared\r\n",
            (unsigned)(STABLE_UPTIME_MS / 60000U));
      }
    }
    if (kick) {
      Boot_WdtKick();
    } else if (!reported) {
      /* With jumper DIS_WDT fitted the board keeps running; otherwise it resets. */
      reported = true;
      (void)DebugLog_Printf("SUP: stopped kicking watchdog (%s%s%s)\r\n", Supervisor_Phase(),
          lostTask >= 0 ? ": " : "", lostTask >= 0 ? taskNames[lostTask] : "");
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
  Supervisor_TaskHeartbeat(SUP_TASK_DEFAULT);
  if (phase == PHASE_BOOT) phase = PHASE_RUN;
}

void Supervisor_TaskHeartbeat(SupTask t)
{
  if ((unsigned)t >= SUP_TASK_COUNT) return;
  lastBeat[t] = xTaskGetTickCount();
  registered[t] = true;
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

const char *Supervisor_LostTask(void)
{
  int t = lostTask;
  return t >= 0 ? taskNames[t] : NULL;
}

uint32_t Supervisor_HeartbeatAgeMs(SupTask t)
{
  if ((unsigned)t >= SUP_TASK_COUNT || !registered[t]) return UINT32_MAX;
  return (uint32_t)((xTaskGetTickCount() - lastBeat[t]) * portTICK_PERIOD_MS);
}

const char *Supervisor_TaskName(SupTask t)
{
  return (unsigned)t < SUP_TASK_COUNT ? taskNames[t] : "?";
}
