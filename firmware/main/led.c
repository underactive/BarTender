// firmware/main/led.c
//
// WS2812 RGB LED driver via espressif/led_strip (RMT backend).
// Lights the board's single on-board LED with the active provider's brand color
// at 90% brightness. Call led_set_provider() from ui_task only (no mutex
// needed — same task as render()).
#include "led.h"
#include "led_strip.h"
#include "board_config.h"
#include "provider_colors.h"

static led_strip_handle_t s_strip;

void led_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num         = BOARD_RGB_LED_GPIO,
        .max_leds               = 1,
        .led_model              = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000,   // 10 MHz — standard for WS2812
    };
    if (led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip) != ESP_OK)
        s_strip = NULL;
    else
        led_strip_clear(s_strip);
}

// sRGB -> linear gamma LUT for WS2812.  Display brand colors are sRGB; LEDs
// respond linearly, so 0xCC (204) on-screen would drive the red diode at 80%
// power instead of the ~60% the eye perceives — all channels bloat toward
// white.  This table (Adafruit WS2812 gamma) maps sRGB 0-255 to the linear
// PWM value that produces the same perceived brightness on the LED.
static const uint8_t GAMMA8[256] = {
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,
      1,  1,  1,  1,  1,  1,  1,  1,  1,  2,  2,  2,  2,  2,  2,  2,
      2,  3,  3,  3,  3,  3,  3,  3,  4,  4,  4,  4,  4,  5,  5,  5,
      5,  6,  6,  6,  6,  7,  7,  7,  7,  8,  8,  8,  9,  9,  9, 10,
     10, 10, 11, 11, 11, 12, 12, 13, 13, 13, 14, 14, 15, 15, 16, 16,
     17, 17, 18, 18, 19, 19, 20, 20, 21, 21, 22, 22, 23, 24, 24, 25,
     25, 26, 27, 27, 28, 29, 29, 30, 31, 32, 32, 33, 34, 35, 35, 36,
     37, 38, 39, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 50,
     51, 52, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 66, 67, 68,
     69, 70, 72, 73, 74, 75, 77, 78, 79, 81, 82, 83, 85, 86, 87, 89,
     90, 92, 93, 95, 96, 98, 99,101,102,104,105,107,109,110,112,114,
    115,117,119,120,122,124,126,127,129,131,133,135,137,138,140,142,
    144,146,148,150,152,154,156,158,160,162,164,167,169,171,173,175,
    177,180,182,184,186,189,191,193,196,198,200,203,205,208,210,213,
    215,218,220,223,225,228,231,233,236,239,241,244,247,249,252,255,
};

void led_set_provider(const char *id)
{
    if (!s_strip) return;
    uint32_t h = prov_color_hex(id);
    if (!h) { led_strip_clear(s_strip); return; }
    // Gamma-correct first (sRGB→linear), then scale to 90% brightness.
    led_strip_set_pixel(s_strip, 0,
        (uint8_t)(GAMMA8[(h >> 16) & 0xFF] * 9 / 10),
        (uint8_t)(GAMMA8[(h >>  8) & 0xFF] * 9 / 10),
        (uint8_t)(GAMMA8[(h      ) & 0xFF] * 9 / 10));
    led_strip_refresh(s_strip);
}

void led_off(void)
{
    if (!s_strip) return;
    led_strip_clear(s_strip);
}
