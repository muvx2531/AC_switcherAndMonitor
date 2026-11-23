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

/* Toggle Clr_wdt (PE7) for the external STWD100 watchdog (timeout 1.12 s).
 * Boot_WdtEarlyKick configures PE7 itself so it can run before MX_GPIO_Init. */
void Boot_WdtEarlyKick(void);
void Boot_WdtKick(void);

/* Put unused interfaces into a no-backfeed state (decisions S2-5, S3-4):
 * USART1 (EC25) and USART2 (RS485) are de-initialized so PA9/PD5 stop driving
 * into unpowered rails; PD1 (LTE power) and PE1 (+3.3V_PER) stay low. */
void Boot_SafeUnusedInterfaces(void);

/* RTC calendar handling: true when the RTC kept its time across this reset. */
void Boot_SetRtcKept(bool kept);
bool Boot_RtcKept(void);

#endif
