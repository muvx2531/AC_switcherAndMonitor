#ifndef BOOT_H
#define BOOT_H
#include <stdbool.h>
#include <stdint.h>

/* Early boot helpers (architecture Phase 1, docs/architecture/tasks/init.md).
 * Called from main() USER CODE sections before the scheduler starts. */

/* Read and clear RCC_CSR reset flags. Call first in main(), before HAL_Init. */
void Boot_CaptureResetCause(void);
/* "POWER_ON", "PIN", "SOFTWARE", "IWDG", "WWDG", "LOW_POWER", "BROWN_OUT" or "UNKNOWN".
 * An external STWD100 watchdog reset appears as "PIN" (it drives NRST). */
const char *Boot_ResetCause(void);
/* 1 LOW_POWER, 2 WWDG, 3 IWDG, 4 SOFTWARE, 5 POWER_ON, 6 BROWN_OUT, 7 PIN, 0 UNKNOWN (event log). */
int Boot_ResetCauseIndex(void);

/* Toggle Clr_wdt (PE7) for the external STWD100 watchdog (timeout 1.12 s).
 * Boot_WdtEarlyKick configures PE7 itself so it can run before MX_GPIO_Init. */
void Boot_WdtEarlyKick(void);
void Boot_WdtKick(void);
/* Longest gap between kicks since boot (or the last reset of the maximum), in ms. */
uint32_t Boot_WdtMaxGapMs(void);
uint32_t Boot_WdtGapNowMs(void);
void Boot_WdtResetMaxGap(void);

/* Put unused interfaces into a no-backfeed state (decisions S2-5, S3-4):
 * USART1 (EC25) and USART2 (RS485) are de-initialized so PA9/PD5 stop driving
 * into unpowered rails; PD1 (LTE power) and PE1 (+3.3V_PER) stay low. */
void Boot_SafeUnusedInterfaces(void);

/* RTC calendar handling: true when the RTC kept its time across this reset. */
void Boot_SetRtcKept(bool kept);
bool Boot_RtcKept(void);

/* Boot-loop detection (decision S6-5). Call once from the default task after the RTC
 * is initialized. Counts resets except POWER_ON in BKP DR5 (count) / DR6 (window start,
 * RTC seconds). With a kept RTC the window is 10 min; without it the count is
 * consecutive and Supervisor clears it after 10 min of uptime. >= 5 -> safe mode:
 * only Control, Power and Supervisor run (no network, storage, acquisition). */
#define BOOT_SAFE_MODE_RESETS 5U
#define BOOT_SAFE_MODE_WINDOW_S 600U
void Boot_EvaluateSafeMode(void);
/* RTC calendar as seconds since 2000-01-01 (meaningful across resets only if Boot_RtcKept). */
uint32_t Boot_RtcSeconds(void);
/* Wall-clock time. The RTC starts at 2000-01-01 on a new board; it is true time only after
 * Boot_SetUnixTime (remote link "time" message), remembered in BKP DR7 while the backup
 * domain keeps power. */
#define BOOT_UNIX_2000 946684800UL
bool Boot_TimeValid(void);
uint32_t Boot_UnixNow(void);                 /* Boot_RtcSeconds() + BOOT_UNIX_2000 */
/* Task context. Rejects times before 2024. Shifts the safe-mode window start by the same
 * step so a clock change is not taken for an expired or endless reset window. */
bool Boot_SetUnixTime(uint32_t unix_s);
bool Boot_SafeMode(void);
uint32_t Boot_ResetCount(void);
/* Clear the reset counter (stable uptime reached, or serial CLRSAFE). Takes effect
 * at the next reset; a running safe mode stays until then. */
void Boot_ClearResetCount(void);

#endif
