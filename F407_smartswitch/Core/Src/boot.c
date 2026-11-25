#include "boot.h"
#include "main.h"

extern RTC_HandleTypeDef hrtc;

#define BKP_RESET_MAGIC 0x52430000U   /* "RC" | count in the low 16 bits */

extern UART_HandleTypeDef huart1;
extern UART_HandleTypeDef huart2;

static uint32_t resetFlags;
static bool rtcKept;
static bool safeMode;
static uint32_t resetCount;

void Boot_CaptureResetCause(void)
{
  resetFlags = RCC->CSR;
  RCC->CSR |= RCC_CSR_RMVF;
}

const char *Boot_ResetCause(void)
{
  /* Order matters: a brown-out/power-on also sets PINRSTF. */
  if (resetFlags & RCC_CSR_LPWRRSTF) return "LOW_POWER";
  if (resetFlags & RCC_CSR_WWDGRSTF) return "WWDG";
  if (resetFlags & RCC_CSR_IWDGRSTF) return "IWDG";
  if (resetFlags & RCC_CSR_SFTRSTF) return "SOFTWARE";
  if (resetFlags & RCC_CSR_PORRSTF) return "POWER_ON";
  if (resetFlags & RCC_CSR_BORRSTF) return "BROWN_OUT";
  if (resetFlags & RCC_CSR_PINRSTF) return "PIN";
  return "UNKNOWN";
}

int Boot_ResetCauseIndex(void)
{
  static const char *const names[] = { "UNKNOWN", "LOW_POWER", "WWDG", "IWDG", "SOFTWARE", "POWER_ON", "BROWN_OUT", "PIN" };
  const char *c = Boot_ResetCause();
  for (int i = 1; i < 8; i++) if (names[i] == c || (names[i][0] == c[0] && names[i][1] == c[1])) return i;
  return 0;
}

void Boot_WdtEarlyKick(void)
{
  __HAL_RCC_GPIOE_CLK_ENABLE();
  GPIO_InitTypeDef gpio = {0};
  gpio.Pin = Clr_wdt_Pin;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(Clr_wdt_GPIO_Port, &gpio);
  Boot_WdtKick();
}

/* Longest interval between two kicks (DWT cycle counter, 168 MHz after SystemClock_Config).
 * The STWD100 resets the MCU after 1.12 s (minimum) without a WDI edge (open item H7). */
static uint32_t lastKickCyc, maxGapCyc;
static bool gapOn;

void Boot_WdtKick(void)
{
  uint32_t now = DWT->CYCCNT;
  if (gapOn) { uint32_t g = now - lastKickCyc; if (g > maxGapCyc) maxGapCyc = g; }
  else {
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    now = DWT->CYCCNT;
    gapOn = true;
  }
  lastKickCyc = now;
  HAL_GPIO_TogglePin(Clr_wdt_GPIO_Port, Clr_wdt_Pin);
}

uint32_t Boot_WdtMaxGapMs(void) { return maxGapCyc / (SystemCoreClock / 1000U); }
uint32_t Boot_WdtGapNowMs(void) { return (DWT->CYCCNT - lastKickCyc) / (SystemCoreClock / 1000U); }
void Boot_WdtResetMaxGap(void) { maxGapCyc = 0; }

void Boot_SafeUnusedInterfaces(void)
{
  /* De-init returns PA9/PA10 and PD5/PD6 to reset (analog) state and stops DMA. */
  (void)HAL_UART_DeInit(&huart1);
  (void)HAL_UART_DeInit(&huart2);
  HAL_GPIO_WritePin(LTE_POW_GPIO_Port, LTE_POW_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(En_3v3_Per_GPIO_Port, En_3v3_Per_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(RS485_DIR_GPIO_Port, RS485_DIR_Pin, GPIO_PIN_RESET);
}

void Boot_SetRtcKept(bool kept) { rtcKept = kept; }

/* Seconds since 2000-01-01 from the RTC shadow registers (24 h format, BCD). */
uint32_t Boot_RtcSeconds(void)
{
  uint32_t tr = RTC->TR, dr = RTC->DR;
  (void)RTC->DR;   /* reading TR locks DR until DR is read: TR then DR is consistent */
  unsigned int ss = ((tr >> 4) & 7U) * 10U + (tr & 15U);
  unsigned int mm = ((tr >> 12) & 7U) * 10U + ((tr >> 8) & 15U);
  unsigned int hh = ((tr >> 20) & 3U) * 10U + ((tr >> 16) & 15U);
  unsigned int d = ((dr >> 4) & 3U) * 10U + (dr & 15U);
  unsigned int m = ((dr >> 12) & 1U) * 10U + ((dr >> 8) & 15U);
  int y = (int)(((dr >> 20) & 15U) * 10U + ((dr >> 16) & 15U)) + 2000;
  /* days from civil (Howard Hinnant) */
  y -= m <= 2U;
  int era = y / 400;
  unsigned int yoe = (unsigned int)(y - era * 400);
  unsigned int doy = (153U * (m + (m > 2U ? (unsigned)-3 : 9U)) + 2U) / 5U + d - 1U;
  unsigned int doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
  long days = (long)era * 146097L + (long)doe - 730425L;   /* 1970 offset 719468 + 10957 days to 2000 */
  return (uint32_t)days * 86400U + hh * 3600U + mm * 60U + ss;
}

void Boot_EvaluateSafeMode(void)
{
  HAL_PWR_EnableBkUpAccess();
  uint32_t head = HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR5);
  uint32_t count = (head & 0xFFFF0000U) == BKP_RESET_MAGIC ? (head & 0xFFFFU) : 0U;
  uint32_t now = rtcKept ? Boot_RtcSeconds() : 0U;
  if (resetFlags & RCC_CSR_PORRSTF) {
    count = 0U;                                 /* power-on is not a reset loop */
  } else if (rtcKept && count > 0U) {
    uint32_t start = HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR6);
    if (now - start > BOOT_SAFE_MODE_WINDOW_S) count = 0U;   /* window expired */
  }
  if (!(resetFlags & RCC_CSR_PORRSTF)) {
    if (count == 0U) HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR6, now);
    if (count < 0xFFFFU) count++;
  }
  HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR5, BKP_RESET_MAGIC | count);
  resetCount = count;
  safeMode = count >= BOOT_SAFE_MODE_RESETS;
}

bool Boot_SafeMode(void) { return safeMode; }
uint32_t Boot_ResetCount(void) { return resetCount; }

void Boot_ClearResetCount(void)
{
  HAL_PWR_EnableBkUpAccess();
  HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR5, BKP_RESET_MAGIC);
}
bool Boot_RtcKept(void) { return rtcKept; }
