#ifndef LOCAL_UI_H
#define LOCAL_UI_H
#include <stdbool.h>

/* Local UI (docs/local-ui-status.md, docs/architecture/tasks/local-ui.md): status LED
 * (PC11) patterns and the SSD1306 128x32 OLED on I2C1 (shared with the PCA9536 through
 * i2c_bus.c). Short press of SW1/SW2: wake the display / next page. Hold either for 5 s:
 * control mode (SW1 selects CH1..CH4/EXIT, hold SW2 2 s toggles ON/OFF through
 * Control_Submit with SRC_LOCAL; web/serial refused meanwhile; leaves after 10 s idle).
 * No control mode without a working display. Start after Control_Start (the first PCA9536 read must come before any
 * OLED traffic, S6-1); runs in safe mode too. An absent or failing OLED is retried every
 * 10 s and never blocks the relays. The task is not supervised: a stuck UI never resets
 * the board. */
bool LocalUi_Start(void);

#endif
