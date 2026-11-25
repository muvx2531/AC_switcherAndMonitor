#ifndef BL_MAIN_H
#define BL_MAIN_H

#define BL_VERSION "1.1.0"

/* Configure PE7 and toggle it once (before HAL_Init): the STWD100 times out after 1.12 s. */
void Bl_EarlyWdtKick(void);
/* Boot decision, install / rollback, then jump to the application. Never returns. */
void Bl_Main(void);

#endif
