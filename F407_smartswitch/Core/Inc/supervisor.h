#ifndef SUPERVISOR_H
#define SUPERVISOR_H
#include <stdbool.h>

/* Minimal Supervisor (Phase 1): the only task that toggles the STWD100 (PE7)
 * after main(). Boot phase: kicks until Supervisor_BootComplete() or the boot
 * deadline. Run phase: kicks only while the default task heartbeat is fresh.
 * Full health registry, BKP reset counters and safe mode come in Phase 4. */
bool Supervisor_Start(void);
void Supervisor_BootComplete(void);
void Supervisor_Heartbeat(void);
/* Control task progress; once called, the run phase also requires it < 2 s old. */
void Supervisor_ControlHeartbeat(void);
/* "boot", "run", "boot_timeout" or "heartbeat_lost". */
const char *Supervisor_Phase(void);

#endif
