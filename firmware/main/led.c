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

#define LED_SUMMARY_TRANSITION_MS  5000
static int64_t s_summary_anchor_ms;
static int     s_summary_n;

// ── Color transition (screensaver provider changes) ──────────────────
#define LED_TRANSITION_MS 700   // matches SCREENSAVER_FADE_MS

typedef struct {
    bool active;
    uint8_t from_r, from_g, from_b;
    uint8_t to_r, to_g, to_b;
    int64_t start_ms, duration_ms;
} led_xfade_t;

static bool     s_transition_enabled;
static led_xfade_t s_xfade;
// Write cache — mirrors the last RGB value pushed to the LED so that
// transitions can interpolate from the current colour.  Updated by
// led_write_rgb(); reset to (0,0,0) by led_off().
static bool    s_last_written;
static uint8_t s_last_r, s_last_g, s_last_b;

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

static void led_write_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) return;
    // Cache the last-written values and skip set_pixel + refresh when
    // they are identical (avoids a blocking RMT TX on every tick with no change).
    if (s_last_written && r == s_last_r && g == s_last_g && b == s_last_b) return;
    s_last_r = r; s_last_g = g; s_last_b = b;
    s_last_written = true;
    led_strip_set_pixel(s_strip, 0,
        (uint8_t)(GAMMA8[r] * 9 / 10),
        (uint8_t)(GAMMA8[g] * 9 / 10),
        (uint8_t)(GAMMA8[b] * 9 / 10));
    led_strip_refresh(s_strip);
}

void led_set_provider(const char *id)
{
    if (!s_strip) return;
    uint32_t h = prov_color_hex(id);
    if (!h) { led_off(); return; }

    uint8_t tr = (h >> 16) & 0xFF, tg = (h >> 8) & 0xFF, tb = h & 0xFF;

    if (s_transition_enabled && s_last_written) {
        // Start smooth interpolation from current colour to new provider colour.
        s_xfade = (led_xfade_t){
            .active   = true,
            .from_r   = s_last_r, .from_g = s_last_g, .from_b = s_last_b,
            .to_r     = tr,       .to_g   = tg,       .to_b   = tb,
            .start_ms = 0,        // set on first tick
            .duration_ms = LED_TRANSITION_MS,
        };
        return;
    }
    led_write_rgb(tr, tg, tb);
}

void led_off(void)
{
    if (!s_strip) return;
    // Use led_write_rgb so the internal cache is updated to (0,0,0);
    // a subsequent transition then starts from black rather than stale colour.
    led_write_rgb(0, 0, 0);
}

void led_summary_reset(void)
{
    s_summary_anchor_ms = 0;
    s_summary_n = -1;
}

void led_summary_cycle_tick(int64_t now_ms, const char *const *ids, int n)
{
    if (!s_strip) return;
    if (n <= 0) {
        led_off();
        led_summary_reset();
        return;
    }
    if (n == 1) {
        led_set_provider(ids[0]);
        led_summary_reset();
        return;
    }
    if (n != s_summary_n) {
        s_summary_n = n;
        s_summary_anchor_ms = now_ms;
    }
    if (s_summary_anchor_ms == 0)
        s_summary_anchor_ms = now_ms;

    const int64_t period = LED_SUMMARY_TRANSITION_MS;
    int64_t phase = (now_ms - s_summary_anchor_ms) % (period * n);
    if (phase < 0) phase += period * n;
    const int idx  = (int)(phase / period);
    const int next = (idx + 1) % n;
    const int64_t t = phase % period;

    uint32_t h0 = prov_color_hex(ids[idx]);
    uint32_t h1 = prov_color_hex(ids[next]);
    if (!h0 || !h1) { led_off(); return; }

    const uint8_t r0 = (h0 >> 16) & 0xFF, g0 = (h0 >> 8) & 0xFF, b0 = h0 & 0xFF;
    const uint8_t r1 = (h1 >> 16) & 0xFF, g1 = (h1 >> 8) & 0xFF, b1 = h1 & 0xFF;
    const uint32_t frac = (uint32_t)(t * 255 / period);   // 0..255
    const uint8_t r = (uint8_t)(r0 + (((int)r1 - (int)r0) * (int)frac) / 255);
    const uint8_t g = (uint8_t)(g0 + (((int)g1 - (int)g0) * (int)frac) / 255);
    const uint8_t b = (uint8_t)(b0 + (((int)b1 - (int)b0) * (int)frac) / 255);
    led_write_rgb(r, g, b);
}

// ── Colour transition API (screensaver provider changes) ─────────────
void led_transition_enable(void) { s_transition_enabled = true; }
bool led_is_transitioning(void) { return s_xfade.active; }
void led_transition_disable(void)
{
    s_transition_enabled = false;
    s_xfade.active = false;
}

void led_transition_tick(int64_t now_ms)
{
    if (!s_xfade.active) return;
    if (s_xfade.start_ms == 0) s_xfade.start_ms = now_ms;
    int64_t elapsed = now_ms - s_xfade.start_ms;
    if (elapsed >= s_xfade.duration_ms) {
        led_write_rgb(s_xfade.to_r, s_xfade.to_g, s_xfade.to_b);
        s_xfade.active = false;
        return;
    }
    uint32_t frac = (uint32_t)(elapsed * 255 / s_xfade.duration_ms);
    uint8_t r = (uint8_t)(s_xfade.from_r +
                   (((int)s_xfade.to_r - (int)s_xfade.from_r) * (int)frac) / 255);
    uint8_t g = (uint8_t)(s_xfade.from_g +
                   (((int)s_xfade.to_g - (int)s_xfade.from_g) * (int)frac) / 255);
    uint8_t b = (uint8_t)(s_xfade.from_b +
                   (((int)s_xfade.to_b - (int)s_xfade.from_b) * (int)frac) / 255);
    led_write_rgb(r, g, b);
}

