// firmware/main/display.h
#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>
#include "lvgl.h"

// Initialize SPI bus, the configured LCD panel (ST7789/ILI9341 per board_config.h), LVGL display, and tick timer.
// Returns the LVGL display object. Starts backlight.
lv_display_t *display_init(void);

// Set backlight brightness (0-255 PWM duty cycle).
void display_set_brightness(uint8_t duty);

// Set PWM backlight brightness without logging; intended for frequent UI-owned
// fade steps. Use display_set_brightness() for user/config-driven changes.
void display_set_brightness_silent(uint8_t duty);

// Rotate the panel 180° in hardware (toggles both mirror flags relative to the
// board's native BOARD_LCD_MIRROR_X/Y). Applied live — no reboot needed. Safe
// to call before display_init() returns (no-op until the panel handle exists).
void display_set_flipped(bool flipped);

// Returns a pointer to the shadow framebuffer (LCD_H_RES × LCD_V_RES, RGB565-LE).
// Populated tile-by-tile during each LVGL flush WHILE shadow capture is enabled
// (see display_shadow_capture). Contents represent the full screen only after
// ui_capture_screenshot() has forced a synchronous re-render. Returns NULL if
// PSRAM is unavailable. The buffer is owned by display.c — do not free it.
const uint8_t *display_get_shadow_fb(int *w, int *h);

// Enable/disable the shadow-framebuffer copy in the LVGL flush path. Disabled by
// default so steady-state frames skip the extra per-flush PSRAM memcpy. The
// screenshot path enables it around ui_capture_screenshot()'s forced full
// re-render, then disables it once the frame is captured.
void display_shadow_capture(bool on);

#endif // DISPLAY_H
