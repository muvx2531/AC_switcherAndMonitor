#include "boot.h"
#include "main.h"

extern RTC_HandleTypeDef hrtc;

#define BKP_RESET_MAGIC 0x52430000U   /* "RC" | count in the low 16 bits */
#define BKP_TIME_MAGIC  0x54494D45U   /* "TIME": RTC set from a trusted source (DR7) */

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

bool Boot_TimeValid(void) { return rtcKept && HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR7) == BKP_TIME_MAGIC; }

uint32_t Boot_UnixNow(void) { return Boot_RtcSeconds() + BOOT_UNIX_2000; }

bool Boot_SetUnixTime(uint32_t unix_s)
{
  if (unix_s < 1704067200UL || unix_s >= BOOT_UNIX_2000 + 99U * 365U * 86400U) return false;  /* 2024..2098 */
  uint32_t before = Boot_RtcSeconds(), s = unix_s - BOOT_UNIX_2000;
  /* civil from days (Howard Hinnant), days since 1970 */
  long z = (long)(unix_s / 86400U) + 719468L;
  long era = z / 146097L;
  unsigned int doe = (unsigned int)(z - era * 146097L);
  unsigned int yoe = (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;
  unsigned int doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);
  unsigned int mp = (5U * doy + 2U) / 153U;
  unsigned int d = doy - (153U * mp + 2U) / 5U + 1U;
  unsigned int m = mp < 10U ? mp + 3U : mp - 9U;
  int y = (int)yoe + (int)era * 400 + (m <= 2U);
  uint32_t sod = unix_s % 86400U;
  RTC_TimeTypeDef t = {0};
  RTC_DateTypeDef dt = {0};
  t.Hours = (uint8_t)(sod / 3600U); t.Minutes = (uint8_t)((sod / 60U) % 60U); t.Seconds = (uint8_t)(sod % 60U);
  t.DayLightSaving = RTC_DAYLIGHTSAVING_NONE; t.StoreOperation = RTC_STOREOPERATION_RESET;
  dt.Year = (uint8_t)(y - 2000); dt.Month = (uint8_t)m; dt.Date = (uint8_t)d;
  dt.WeekDay = (uint8_t)(((unix_s / 86400U) + 3U) % 7U + 1U);   /* 1970-01-01 was a Thursday; Monday = 1 */
  HAL_PWR_EnableBkUpAccess();
  if (HAL_RTC_SetTime(&hrtc, &t, RTC_FORMAT_BIN) != HAL_OK) return false;
  if (HAL_RTC_SetDate(&hrtc, &dt, RTC_FORMAT_BIN) != HAL_OK) return false;
  uint32_t start = HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR6);
  HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR6, start + (s - before));
  HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR7, BKP_TIME_MAGIC);
  return true;
}
