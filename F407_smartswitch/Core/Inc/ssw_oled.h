/* SSD1306 128x32 OLED: frame buffer renderer and command sequences.
 * Identical copy in Bootloader/ and F407_smartswitch/ (like ssw_image.h).
 *
 * No I2C code here: the caller passes a write function. The bootloader uses blocking
 * HAL calls, the application goes through i2c_bus.c (shared with the PCA9536).
 * Each write is one I2C transaction to address SSW_OLED_ADDR: control byte `ctrl`
 * (0x00 = commands, 0x40 = display data) followed by `len` bytes (len <= 128). */
#ifndef SSW_OLED_H
#define SSW_OLED_H
#include <stdbool.h>
#include <stdint.h>

#define SSW_OLED_ADDR     0x3CU
#define SSW_OLED_W        128U
#define SSW_OLED_PAGES    4U                       /* 4 x 8 px = 32 px */
#define SSW_OLED_FB_SIZE  (SSW_OLED_W * SSW_OLED_PAGES)
#define SSW_OLED_COLS     21U                      /* 6 px per character */
#define SSW_OLED_CTRL_CMD  0x00U
#define SSW_OLED_CTRL_DATA 0x40U

typedef bool (*SswOledWrite)(uint8_t ctrl, const uint8_t *data, uint16_t len);

/* ---- panel ---- */
bool SswOled_Init(SswOledWrite w, uint8_t contrast);       /* full init, display on */
bool SswOled_Contrast(SswOledWrite w, uint8_t contrast);
bool SswOled_Power(SswOledWrite w, bool on);
/* Send pages first..last (0..3) of the frame buffer. */
bool SswOled_Flush(SswOledWrite w, const uint8_t *fb, unsigned first, unsigned last);

/* ---- drawing into a SSW_OLED_FB_SIZE buffer (bit 0 = top pixel of a page) ---- */
void SswOled_Clear(uint8_t *fb);
void SswOled_ClearLine(uint8_t *fb, unsigned line);
/* 5x7 text on text line 0..3 starting at pixel x; returns the x after the text.
 * invert: white background box (1 px margin). Characters outside 32..126 show '?'. */
unsigned SswOled_Text(uint8_t *fb, unsigned x, unsigned line, const char *s, bool invert);
/* Double size text (12 x 16 px per character) on lines line and line+1. */
unsigned SswOled_TextBig(uint8_t *fb, unsigned x, unsigned line, const char *s);
/* Progress bar on one text line: outline from x to x+w-1, filled permille/1000. */
void SswOled_Bar(uint8_t *fb, unsigned x, unsigned line, unsigned w, unsigned permille);

#endif
