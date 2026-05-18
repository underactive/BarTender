// firmware/main/board_config.h
//
// Central per-board hardware configuration: pins, geometry, and capability
// flags. Every file that touches hardware reads its constants from here, so
// adding a new board is a matter of adding a new #ifdef block.
//
// Selection is driven by the Kconfig choice `CLAWD_BOARD`
// (see firmware/main/Kconfig.projbuild).

#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

// The simulator shares scene.c / notification_ui.c / ui_manager.c from this
// directory and defines SIMULATOR at compile time. It doesn't run on ESP-IDF,
// so sdkconfig.h doesn't exist — we skip the include and pin the simulator
// to the C6 geometry by default (matches existing scenario expectations).
#ifndef SIMULATOR
#include "sdkconfig.h"
#endif

// ============================================================================
// Simulator (native macOS/Linux via SDL2 — no real hardware)
// ============================================================================
#if defined(SIMULATOR)

// Scene geometry the simulator pretends to drive. Default is the C6 panel
// size so existing scenarios render identically. Flip to 320x240 to preview
// the fnk0104 layout before flashing hardware.
#define BOARD_SIM_HEIGHT_C6    1   // 1 = 172-row layout, 0 = 240-row layout
#define BOARD_LCD_H_RES        320
#if BOARD_SIM_HEIGHT_C6
#define BOARD_LCD_V_RES        172
#else
#define BOARD_LCD_V_RES        240
#endif

#define BOARD_HAS_BOOT_BUTTON  0
#define BOARD_HAS_TOUCH        0
#define BOARD_HAS_PSRAM        0
#define BOARD_HAS_I2C_BUS      0
#define BOARD_HAS_AUDIO        0

// RGB LED GPIO is ignored by the simulator's led_strip shim (see
// simulator/shims/led_strip.h — the shim routes colors to
// sim_rgb_led_update() instead). Any value compiles; keep the C6 pin for
// consistency with the default scene geometry above.
#define BOARD_RGB_LED_GPIO     8

// ============================================================================
// Waveshare ESP32-C6-LCD-1.47
// ============================================================================
#elif defined(CONFIG_BOARD_WAVESHARE_C6_LCD147)

// --- Display (ST7789, 320x172 landscape, centered in 240-row controller RAM)
#define BOARD_LCD_DRIVER_ST7789  1
#define BOARD_LCD_H_RES          320
#define BOARD_LCD_V_RES          172
#define BOARD_LCD_PIXEL_CLK_HZ   (12 * 1000 * 1000)
#define BOARD_LCD_GAP_X          0
#define BOARD_LCD_GAP_Y          34   // 172 rows centered in 240-row RAM
#define BOARD_LCD_INVERT_COLOR   1
#define BOARD_LCD_SWAP_XY        1
#define BOARD_LCD_MIRROR_X       1
#define BOARD_LCD_MIRROR_Y       0
#define BOARD_LCD_RGB_ORDER_BGR  0   // RGB element order
#define BOARD_LCD_SATURATION_BOOST_X16  16  // off — TN panel is already punchy

#define BOARD_LCD_PIN_MOSI   6
#define BOARD_LCD_PIN_SCLK   7
#define BOARD_LCD_PIN_MISO   5   // unused but required by SPI bus config
#define BOARD_LCD_PIN_CS    14
#define BOARD_LCD_PIN_DC    15
#define BOARD_LCD_PIN_RST   21
#define BOARD_LCD_PIN_BL    22

// --- RGB LED
#define BOARD_RGB_LED_GPIO   8

// --- User input
#define BOARD_HAS_BOOT_BUTTON   1
#define BOARD_BOOT_BUTTON_GPIO  9   // ESP32-C6 BOOT button

// --- Capability flags (what this board lacks)
#define BOARD_HAS_TOUCH     0
#define BOARD_HAS_PSRAM     0
#define BOARD_HAS_I2C_BUS   0
#define BOARD_HAS_AUDIO     0

// ============================================================================
// Freenove ESP32-S3 2.8" (fnk0104)
// ============================================================================
#elif defined(CONFIG_BOARD_FREENOVE_S3_28)

// --- Display (ILI9341, 320x240 landscape, full controller RAM, no gap)
#define BOARD_LCD_DRIVER_ILI9341  1
#define BOARD_LCD_H_RES          320
#define BOARD_LCD_V_RES          240
#define BOARD_LCD_PIXEL_CLK_HZ   (40 * 1000 * 1000)
#define BOARD_LCD_GAP_X          0
#define BOARD_LCD_GAP_Y          0
#define BOARD_LCD_INVERT_COLOR   1   // TFT_INVERSION_ON in the reference
// Orientation/mirror flags are BRING-UP TODO — values below are the most
// likely landscape configuration (TFT_eSPI setRotation(1) equivalent) but
// must be confirmed on hardware. If the image is upside-down, flip
// BOARD_LCD_MIRROR_X and BOARD_LCD_MIRROR_Y together.
#define BOARD_LCD_SWAP_XY        1
#define BOARD_LCD_MIRROR_X       0
#define BOARD_LCD_MIRROR_Y       0
// ILI9341 modules vary: some are RGB, some BGR. The fnk0104's panel is BGR —
// confirmed at bring-up (Clawd rendered blue instead of orange without this).
#define BOARD_LCD_RGB_ORDER_BGR  1

// Saturation boost applied per-pixel in the LVGL flush callback. The fnk0104's
// IPS panel renders colors more accurately than the original Waveshare ST7789
// TN panel, which makes Clawd's orange look comparatively muted. Bump
// saturation to bring the "punch" back. Fixed-point: 16 == 1.00x (off), 20 ==
// 1.25x (mild), 22 == 1.375x (moderate), 24 == 1.50x (strong), 28 == 1.75x
// (very strong). Cost is ~15 integer ops per pixel; partial-render mode keeps
// total work well under 5 ms per full flush.
#define BOARD_LCD_SATURATION_BOOST_X16  26

#define BOARD_LCD_PIN_MOSI   11
#define BOARD_LCD_PIN_SCLK   12
#define BOARD_LCD_PIN_MISO   13   // present on the ribbon; unused by write-only path
#define BOARD_LCD_PIN_CS     10
#define BOARD_LCD_PIN_DC     46
#define BOARD_LCD_PIN_RST    -1   // no dedicated reset line; panel uses POR
#define BOARD_LCD_PIN_BL     45

// --- RGB LED
#define BOARD_RGB_LED_GPIO   42

// --- User input (no BOOT button exposed; dismiss via capacitive touch)
#define BOARD_HAS_BOOT_BUTTON   0

// --- Shared I2C master bus (touch + audio codec)
#define BOARD_HAS_I2C_BUS       1
#define BOARD_I2C_SDA           16
#define BOARD_I2C_SCL           15

// --- Capacitive touch (FT6336G, FT5x06-family protocol, on the shared I2C bus)
#define BOARD_HAS_TOUCH         1
#define BOARD_TOUCH_INT_GPIO    21
#define BOARD_TOUCH_RST_GPIO    -1
#define BOARD_TOUCH_I2C_ADDR    0x38
#define BOARD_TOUCH_NATIVE_W    240   // raw panel reports portrait coordinates
#define BOARD_TOUCH_NATIVE_H    320

// --- Audio (ES8311 codec + speaker amp on I2S, same bus as touch)
// Wake-word / mic are unused here — TX-only (playback) configuration.
#define BOARD_HAS_AUDIO             1
#define BOARD_AUDIO_I2S_MCK         4
#define BOARD_AUDIO_I2S_BCK         5
#define BOARD_AUDIO_I2S_WS          7
#define BOARD_AUDIO_I2S_DOUT        8
#define BOARD_AUDIO_AMP_ENABLE_GPIO 1       // active-low (AP_ENABLE): drive LOW to unmute
#define BOARD_AUDIO_AMP_ACTIVE_LOW  1
#define BOARD_AUDIO_CODEC_I2C_ADDR  0x18    // ES8311 with CE tied low
#define BOARD_AUDIO_SAMPLE_RATE     24000
#define BOARD_AUDIO_MCLK_MULT       256
#define BOARD_AUDIO_VOLUME_PCT      40

// --- Memory
#define BOARD_HAS_PSRAM   1

#else
#  error "No supported board selected. Enable CONFIG_BOARD_WAVESHARE_C6_LCD147 or CONFIG_BOARD_FREENOVE_S3_28 in sdkconfig."
#endif

// ============================================================================
// Derived constants (common across boards)
// ============================================================================

// Scene uses the full display for geometry. Exposed here so notification_ui.c,
// scene.c, and ui_manager.c share a single source of truth.
#define BOARD_SCENE_WIDTH   BOARD_LCD_H_RES
#define BOARD_SCENE_HEIGHT  BOARD_LCD_V_RES

#endif // BOARD_CONFIG_H
