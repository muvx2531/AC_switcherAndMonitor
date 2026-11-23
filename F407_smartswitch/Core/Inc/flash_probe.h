#ifndef FLASH_PROBE_H
#define FLASH_PROBE_H

/* Read-only bring-up: identify the two SPI flashes and inspect a candidate sector. */
void FlashProbe_Run(void);
/* One-shot test only: programs and erases a sector after proving it is blank. */
void FlashProbe_TestBlankSectors(void);

#endif
