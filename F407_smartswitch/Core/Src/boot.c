#include "boot.h"
#include "main.h"

extern UART_HandleTypeDef huart1;
extern UART_HandleTypeDef huart2;

static uint32_t resetFlags;
static bool rtcKept;

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

void Boot_WdtKick(void)
{
  HAL_GPIO_TogglePin(Clr_wdt_GPIO_Port, Clr_wdt_Pin);
}

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
bool Boot_RtcKept(void) { return rtcKept; }
