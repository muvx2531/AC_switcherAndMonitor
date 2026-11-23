#ifndef ADC_RAW_H
#define ADC_RAW_H
#include <stdbool.h>
#include <stdint.h>

/* Temporary single owner of ADC2 continuous 10-rank circular DMA (bring-up).
 * Replaced by the Acquisition service (docs/architecture/tasks/acquisition.md).
 * Task context only. The first call starts the DMA and waits up to 40 ms. */
bool AdcRaw_GetVbat(uint16_t *raw);

/* Estimate only: VREF+ = 3000 mV, divider 56k/(49.9k+49.9k+56k). Not calibrated. */
uint32_t AdcRaw_VbatEstimateMv(uint16_t raw);

#endif
