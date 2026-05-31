// firmware/main/touch.c
//
// FT6336G capacitive touch driver. Wraps the espressif/esp_lcd_touch_ft5x06
// managed component (FT6336G shares the FT5x06 register layout).
//
// Strategy: poll-based, not interrupt-driven. A dedicated low-priority task
// wakes every 20 ms, asks the driver to refresh its internal cache, and
// checks for a fresh touch-down. This is simpler than wiring the INT line
// into a GPIO ISR, and 50 Hz is more than fast enough for "did the user tap?"
// UX. If power draw becomes a concern, the INT pin can be swapped in later
// as a wakeup source.
//
// A debounced press is classified into one APP_EVT per touch: TAP (open /
// cycle a page), SWIPE_UP / SWIPE_DOWN (scroll the summary list), SWIPE_LEFT
// (nav "back"), or LONG_PRESS (~1.5 s hold → non-destructive add-network
// portal, handled in fetch.c). Exactly one event fires per press; the bands
// are deliberately disjoint (see the gesture-constant block below).

#include "touch.h"
#include "board_config.h"

#if BOARD_HAS_TOUCH

#include "app_event.h"
#include "i2c_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "touch";

#define POLL_PERIOD_MS  20      // 50 Hz
#define DEBOUNCE_US     200000  // 200 ms minimum between accepted TAPS
                                // (contact-bounce floor; the old triple-tap
                                // detector that depended on this exact value
                                // was removed with the nav redesign).

// Gesture classification. Bands are deliberately DISJOINT with a dead-zone:
// a tap is small+short; a swipe is long+fast+vertical; 12..40 px of travel is
// inert (ambiguous => no event, never a misfire). Tuned for the 20 ms poll,
// ~240x320 panel, and FT6336G jitter (~3-5 px).
#define SWIPE_DIST_PX       40
#define SWIPE_MAX_US        600000   // a flick is fast; rejects slow drags
#define TAP_SLOP_PX         12       // contact wobble; < SWIPE_DIST_PX (no overlap)
#define TAP_MAX_US          700000   // rejects a long rest-press as a tap
#define GESTURE_DEBOUNCE_US 400000   // one press => at most one swipe
#define RELEASE_GRACE       2        // polls of no-touch before commit (FT6336G
                                     // drops a sample mid-glide; 40 ms grace)
#define LONG_PRESS_US     1500000    // held >= 1.5 s within TAP_SLOP => the
                                     // add-network portal gesture (> TAP_MAX_US
                                     // so it can never also read as a tap)

static QueueHandle_t s_queue;
static esp_lcd_touch_handle_t s_touch;
static int64_t s_last_press_us;      // last accepted TAP (contact-bounce floor)
static int64_t s_last_gesture_us;    // last accepted SWIPE
static int64_t s_press_us;           // current press-down timestamp
static bool    s_in_press;
static bool    s_gesture_fired;      // a swipe/long-press already this press
                                     // (also suppresses the release-TAP)
static int     s_press_x, s_press_y; // coords at press-down
static int     s_last_x, s_last_y;   // most recent coords while held
static int     s_release_misses;

static inline int iabs_(int v) { return v < 0 ? -v : v; }

static void touch_task(void *arg)
{
    (void)arg;
    while (1) {
        if (esp_lcd_touch_read_data(s_touch) == ESP_OK) {
            uint16_t x[1] = {0};
            uint16_t y[1] = {0};
            uint16_t strength[1] = {0};
            uint8_t count = 0;
            bool pressed = esp_lcd_touch_get_coordinates(s_touch, x, y, strength,
                                                         &count, 1) && count > 0;
            int64_t now = esp_timer_get_time();

            if (pressed) {
                s_release_misses = 0;
                if (!s_in_press) {
                    // idle -> press: start tracking the gesture
                    s_in_press = true;
                    s_gesture_fired = false;
                    s_press_us = now;
                    s_press_x = s_last_x = x[0];
                    s_press_y = s_last_y = y[0];
                } else {
                    // held: fire long-press / swipe on threshold crossing
                    // (don't wait for release — better felt latency).
                    // Strict precedence so exactly one gesture fires per
                    // press; bands stay disjoint (tap <=12px, swipe >=40px,
                    // 12..40 inert), long-press needs sustained low travel.
                    s_last_x = x[0];
                    s_last_y = y[0];
                    if (!s_gesture_fired) {
                        int dx = s_last_x - s_press_x;
                        int dy = s_last_y - s_press_y;
                        // 1) long-press: still within slop after 1.5 s. Sets
                        //    s_gesture_fired so the release can't also TAP
                        //    (and LONG_PRESS_US > TAP_MAX_US anyway).
                        if (iabs_(dx) <= TAP_SLOP_PX &&
                            iabs_(dy) <= TAP_SLOP_PX &&
                            (now - s_press_us) >= LONG_PRESS_US) {
                            app_evt_t e = { .type = APP_EVT_LONG_PRESS,
                                            .x = (int16_t)s_press_x,
                                            .y = (int16_t)s_press_y };
                            // Audit R&C§P1-1: the queue (depth 8, timeout-0
                            // sends) is frozen during a blocking do_fetch, so
                            // a burst of nav events could fill it and DROP
                            // this — the only on-device recovery gesture. The
                            // portal intent supersedes any queued scroll/tap,
                            // so on a full queue, flush and re-send.
                            if (xQueueSend(s_queue, &e, 0) != pdTRUE) {
                                xQueueReset(s_queue);
                                xQueueSend(s_queue, &e, 0);
                            }
                            s_gesture_fired = true;   // blocks re-fire + tap
                            s_last_gesture_us = now;
                            ESP_LOGD(TAG, "long-press (%d, %d)",
                                     s_press_x, s_press_y);
                        // 2) vertical swipe (unchanged).
                        } else if (iabs_(dy) >= SWIPE_DIST_PX &&
                            iabs_(dy) > iabs_(dx) &&            // vertical-dominant
                            (now - s_press_us) <= SWIPE_MAX_US &&
                            (now - s_last_gesture_us) >= GESTURE_DEBOUNCE_US) {
                            app_evt_t e = {
                                .type = (dy > 0) ? APP_EVT_SWIPE_DOWN
                                                 : APP_EVT_SWIPE_UP,
                                .x = 0, .y = 0,
                            };
                            // Audit (Frontend§MED): unlike the long-press above
                            // we DON'T flush-and-resend here — a reset would drop
                            // a queued long-press (the sole recovery gesture). A
                            // dropped scroll is benign; just make it diagnosable.
                            if (xQueueSend(s_queue, &e, 0) != pdTRUE)
                                ESP_LOGW(TAG, "swipe %s dropped (queue full)",
                                         dy > 0 ? "down" : "up");
                            s_gesture_fired = true;
                            s_last_gesture_us = now;
                            ESP_LOGD(TAG, "swipe %s",
                                     dy > 0 ? "down" : "up");
                        // 3) horizontal swipe, right->left only (nav "back").
                        } else if (iabs_(dx) >= SWIPE_DIST_PX &&
                            iabs_(dx) > iabs_(dy) &&            // horizontal-dominant
                            dx < 0 &&                           // right -> left
                            (now - s_press_us) <= SWIPE_MAX_US &&
                            (now - s_last_gesture_us) >= GESTURE_DEBOUNCE_US) {
                            app_evt_t e = { .type = APP_EVT_SWIPE_LEFT,
                                            .x = 0, .y = 0 };
                            if (xQueueSend(s_queue, &e, 0) != pdTRUE)
                                ESP_LOGW(TAG, "swipe left dropped (queue full)");
                            s_gesture_fired = true;
                            s_last_gesture_us = now;
                            ESP_LOGD(TAG, "swipe left");
                        }
                    }
                }
            } else if (s_in_press) {
                // FT6336G can drop one sample mid-glide; only commit the
                // release after RELEASE_GRACE consecutive no-touch polls.
                if (++s_release_misses >= RELEASE_GRACE) {
                    s_in_press = false;
                    if (!s_gesture_fired) {
                        int dx = s_last_x - s_press_x;
                        int dy = s_last_y - s_press_y;
                        if (iabs_(dx) <= TAP_SLOP_PX &&
                            iabs_(dy) <= TAP_SLOP_PX &&
                            (now - s_press_us) <= TAP_MAX_US &&
                            (now - s_last_press_us) >= DEBOUNCE_US) {
                            s_last_press_us = now;
                            app_evt_t e = {
                                .type = APP_EVT_TAP,
                                .x = (int16_t)s_press_x,
                                .y = (int16_t)s_press_y,
                            };
                            if (xQueueSend(s_queue, &e, 0) != pdTRUE)
                                ESP_LOGW(TAG, "tap dropped (queue full)");
                            ESP_LOGD(TAG, "tap (%d, %d)",
                                     s_press_x, s_press_y);
                        }
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}

void touch_init(QueueHandle_t evt_queue)
{
    s_queue = evt_queue;
    s_last_press_us = 0;
    s_last_gesture_us = 0;
    s_in_press = false;
    s_gesture_fired = false;
    s_release_misses = 0;

    /* Shared I2C master bus — also used by the ES8311 codec (sound.c). */
    i2c_master_bus_handle_t bus = i2c_bus_get();
    if (!bus) {
        ESP_LOGE(TAG, "Failed to acquire shared I2C bus");
        return;
    }

    /* LCD panel IO (I2C) — the esp_lcd abstraction over the FT5x06 register
     * protocol. The ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG macro supplies the
     * correct slave address, bit widths, and control-byte format. */
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io));

    /* Touch driver.
     *
     * Landscape mapping: the FT6336G reports raw portrait coordinates in a
     * 240x320 coordinate frame. We rotate the LCD panel to landscape (320x240)
     * via ESP-IDF's panel driver, so touch needs the same rotation applied
     * here. swap_xy=1 is the first step; mirror flags may need a flip
     * depending on the physical board orientation. Values below mirror the
     * LCD panel orientation flags in board_config.h — keep them in sync. */
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_TOUCH_NATIVE_W,  /* raw panel width — swap_xy flips the axes */
        .y_max = BOARD_TOUCH_NATIVE_H,
        .rst_gpio_num = BOARD_TOUCH_RST_GPIO,
        .int_gpio_num = -1,  /* poll-based; INT not wired for now */
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = {
            .swap_xy  = BOARD_LCD_SWAP_XY,
            .mirror_x = BOARD_LCD_MIRROR_X,
            .mirror_y = BOARD_LCD_MIRROR_Y,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_ft5x06(io, &tp_cfg, &s_touch));

    BaseType_t ret = xTaskCreate(touch_task, "touch", 3072, NULL, 3, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create touch task");
        return;
    }

    ESP_LOGI(TAG, "FT6336G touch initialized (I2C SDA=%d SCL=%d, addr=0x%02x)",
             BOARD_I2C_SDA, BOARD_I2C_SCL, BOARD_TOUCH_I2C_ADDR);
}

void touch_set_flipped(bool flipped)
{
    if (!s_touch) return;
    /* Mirror touch axes to match the 180° display flip in display_set_flipped.
     * Same XOR logic: base orientation uses BOARD_LCD_MIRROR_X/Y; flipped
     * state inverts both so tap coordinates track the panel scan order. */
    bool mx = BOARD_LCD_MIRROR_X ? !flipped : flipped;
    bool my = BOARD_LCD_MIRROR_Y ? !flipped : flipped;
    esp_lcd_touch_set_mirror_x(s_touch, mx);
    esp_lcd_touch_set_mirror_y(s_touch, my);
}

#else  /* !BOARD_HAS_TOUCH */

void touch_init(QueueHandle_t evt_queue) { (void)evt_queue; }
void touch_set_flipped(bool flipped)     { (void)flipped; }

#endif
