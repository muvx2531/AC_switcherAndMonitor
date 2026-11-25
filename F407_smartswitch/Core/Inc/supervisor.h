#ifndef SUPERVISOR_H
#define SUPERVISOR_H
#include <stdbool.h>
#include <stdint.h>

/* Supervisor (docs/architecture/tasks/supervisor.md): the only task that toggles the
 * STWD100 (PE7) after main(). Boot phase: kicks until Supervisor_BootComplete() or the
 * boot deadline. Run phase: kicks only while every registered task heartbeat is fresh
 * (< 2 s). A task is registered by its first heartbeat. After 10 min of run phase the
 * BKP reset counter is cleared (boot-loop detection, Boot_EvaluateSafeMode). */
typedef enum {
  SUP_TASK_DEFAULT = 0,
  SUP_TASK_CONTROL,
  SUP_TASK_ACQ,
  SUP_TASK_POWER,
  SUP_TASK_STORAGE,
  SUP_TASK_COUNT
} SupTask;

bool Supervisor_Start(void);
/* Watchdog test (serial WDTTEST): the default task stops sending heartbeats, so the
 * Supervisor stops kicking after 2 s and the STWD100 resets the board 1.12 s later
 * (with jumper DIS_WDT fitted it only logs). Relays keep their state (I1). */
void Supervisor_WdtTest(void);
bool Supervisor_WdtTestActive(void);
/* Longest watchdog kick gap measured during the boot phase, ms. */
uint32_t Supervisor_BootGapMs(void);
void Supervisor_BootComplete(void);
void Supervisor_TaskHeartbeat(SupTask t);
static inline void Supervisor_Heartbeat(void) { Supervisor_TaskHeartbeat(SUP_TASK_DEFAULT); }
static inline void Supervisor_ControlHeartbeat(void) { Supervisor_TaskHeartbeat(SUP_TASK_CONTROL); }
/* "boot", "run", "boot_timeout" or "heartbeat_lost". */
const char *Supervisor_Phase(void);
/* Name of the task whose heartbeat was lost, or NULL. */
const char *Supervisor_LostTask(void);
/* Age in ms of a task heartbeat; UINT32_MAX when the task never registered. */
uint32_t Supervisor_HeartbeatAgeMs(SupTask t);
const char *Supervisor_TaskName(SupTask t);

#endif
