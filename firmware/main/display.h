// firmware/main/display.h
#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>
#include "lvgl.h"

// Initialize SPI bus, ST7789 panel, LVGL display, and tick timer.
// Returns the LVGL display object. Starts backlight.
lv_display_t *display_init(void);

// Set backlight brightness (0-255 PWM duty cycle).
void display_set_brightness(uint8_t duty);

// Rotate the panel 180° in hardware (toggles both mirror flags relative to the
// board's native BOARD_LCD_MIRROR_X/Y). Applied live — no reboot needed. Safe
// to call before display_init() returns (no-op until the panel handle exists).
void display_set_flipped(bool flipped);

#endif // DISPLAY_H
