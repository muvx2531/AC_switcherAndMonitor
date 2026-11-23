#include "adc_raw.h"
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

extern ADC_HandleTypeDef hadc2;

#define ADC_RANKS 10U
#define VBAT_RANK_INDEX 8U  /* rank 9: ADC_CHANNEL_13 (PC3) */

/* DMA target: ordinary static SRAM, never CCM. */
static volatile uint16_t samples[ADC_RANKS];
static bool started;
static SemaphoreHandle_t startMutex;
static StaticSemaphore_t startMutexStorage;

static bool ensureStarted(void)
{
  if (started) return true;
  taskENTER_CRITICAL();
  if (startMutex == NULL) startMutex = xSemaphoreCreateMutexStatic(&startMutexStorage);
  taskEXIT_CRITICAL();
  if (startMutex == NULL || xSemaphoreTake(startMutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  bool ok = started;
  if (!ok && (HAL_ADC_GetState(&hadc2) & HAL_ADC_STATE_REG_BUSY) == 0U) {
    samples[VBAT_RANK_INDEX] = 0xffffU;
    ok = HAL_ADC_Start_DMA(&hadc2, (uint32_t *)samples, ADC_RANKS) == HAL_OK;
    started = ok;
  }
  xSemaphoreGive(startMutex);
  return ok;
}

bool AdcRaw_GetVbat(uint16_t *raw)
{
  if (raw == NULL || __get_IPSR() != 0U || !ensureStarted()) return false;
  for (unsigned int i = 0; i < 40U && samples[VBAT_RANK_INDEX] == 0xffffU; ++i) vTaskDelay(pdMS_TO_TICKS(1));
  uint16_t value = samples[VBAT_RANK_INDEX];
  if (value > 4095U || HAL_ADC_GetError(&hadc2) != HAL_ADC_ERROR_NONE) return false;
  *raw = value;
  return true;
}

uint32_t AdcRaw_VbatEstimateMv(uint16_t raw)
{
  return (uint32_t)(((uint64_t)raw * 3000U * 1558U + (4095U * 560U) / 2U) / (4095U * 560U));
}
