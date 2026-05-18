// firmware/main/display.c
#include "display.h"
#include "board_config.h"
#include "config_store.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#if defined(BOARD_LCD_DRIVER_ILI9341)
#include "esp_lcd_ili9341.h"
#endif
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "display";

/* Panel handle hoisted to module scope so display_set_flipped() can re-apply
 * the hardware mirror flags after init (see display_set_flipped below). Stays
 * NULL until display_init() finishes the panel-driver setup. */
static esp_lcd_panel_handle_t s_panel = NULL;

/* LVGL display handle — set once display_init has created and registered it.
 * Guards the runtime invalidate in display_set_flipped so the boot-time call
 * (before lv_init) skips the LVGL side. */
static lv_display_t *s_lv_display = NULL;

// Display config
#define LCD_HOST        SPI2_HOST
#define LCD_H_RES       BOARD_LCD_H_RES
#define LCD_V_RES       BOARD_LCD_V_RES
#define LCD_PIXEL_CLK   BOARD_LCD_PIXEL_CLK_HZ
#define LCD_CMD_BITS    8
#define LCD_PARAM_BITS  8
#define LVGL_BUF_LINES  20
#define LVGL_TICK_MS    2

static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                                     esp_lcd_panel_io_event_data_t *edata,
                                     void *user_ctx) {
    lv_display_t *disp = (lv_display_t *)user_ctx;
    lv_display_flush_ready(disp);
    return false;
}

#if BOARD_LCD_SATURATION_BOOST_X16 != 16
/* In-place saturation boost on a span of RGB565 pixels (native byte order,
 * applied before the SPI byte-swap). Per pixel: split into 8-bit channels,
 * compute Rec. 601 luma, push each channel away from luma by `boost/16`,
 * clamp, recompress. ~15 integer ops per pixel; runs in <5 ms for a full
 * 320x240 flush on the dual-core S3, and most flushes are partial. */
static void saturate_rgb565_span(uint16_t *px, int n) {
    const int boost = BOARD_LCD_SATURATION_BOOST_X16;
    for (int i = 0; i < n; i++) {
        uint16_t v = px[i];
        int r = ((v >> 11) & 0x1F) << 3;  // 5 -> 8 bits
        int g = ((v >>  5) & 0x3F) << 2;  // 6 -> 8 bits
        int b = ( v        & 0x1F) << 3;  // 5 -> 8 bits
        int luma = (r * 77 + g * 150 + b * 29) >> 8;  // weights sum to 256
        r = luma + (((r - luma) * boost) >> 4);
        g = luma + (((g - luma) * boost) >> 4);
        b = luma + (((b - luma) * boost) >> 4);
        if (r < 0) r = 0; else if (r > 255) r = 255;
        if (g < 0) g = 0; else if (g > 255) g = 255;
        if (b < 0) b = 0; else if (b > 255) b = 255;
        px[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }
}
#endif

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area,
                           uint8_t *px_map) {
    esp_lcd_panel_handle_t panel = lv_display_get_user_data(disp);

    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;

#if BOARD_LCD_SATURATION_BOOST_X16 != 16
    /* Apply saturation in native RGB565 order, BEFORE the SPI byte-swap. */
    saturate_rgb565_span((uint16_t *)px_map, w * h);
#endif

    lv_draw_sw_rgb565_swap(px_map, w * h);

    esp_lcd_panel_draw_bitmap(panel,
        area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
}

static void lvgl_tick_cb(void *arg) {
    lv_tick_inc(LVGL_TICK_MS);
}

lv_display_t *display_init(void) {
    ESP_LOGI(TAG, "Initializing display...");

    // PWM backlight via LEDC — keep duty low to reduce heat
    ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&bl_timer));
    ledc_channel_config_t bl_channel = {
        .gpio_num = BOARD_LCD_PIN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,  // off during init
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&bl_channel));

    // SPI bus
    spi_bus_config_t buscfg = {
        .sclk_io_num = BOARD_LCD_PIN_SCLK,
        .mosi_io_num = BOARD_LCD_PIN_MOSI,
        .miso_io_num = BOARD_LCD_PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LVGL_BUF_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // Panel I/O
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = BOARD_LCD_PIN_DC,
        .cs_gpio_num = BOARD_LCD_PIN_CS,
        .pclk_hz = LCD_PIXEL_CLK,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, &io_handle));

    // Panel driver (board-selected) — stored at module scope so the runtime
    // display_set_flipped() hook can re-apply mirror flags after init.
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BOARD_LCD_PIN_RST,
#if BOARD_LCD_RGB_ORDER_BGR
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
#else
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
#endif
        .bits_per_pixel = 16,
    };
#if defined(BOARD_LCD_DRIVER_ST7789)
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &s_panel));
#elif defined(BOARD_LCD_DRIVER_ILI9341)
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io_handle, &panel_config, &s_panel));
#else
#  error "No LCD driver selected in board_config.h"
#endif

    // Reset is a no-op when reset_gpio_num = -1 (POR-only panels)
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
#if BOARD_LCD_INVERT_COLOR
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
#endif

    // Landscape orientation — per-board flags (tune during bring-up if wrong).
    // display_set_flipped(stored) below XORs the mirror flags for the user's
    // saved 180° preference.
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, BOARD_LCD_SWAP_XY));
    display_set_flipped(config_store_get_display_flipped());

    // Offset in controller RAM. ST7789 + 172-row display needs y_gap=34 to
    // center the visible window; ILI9341 fills the 320x240 RAM so gap is 0.
#if (BOARD_LCD_GAP_X != 0) || (BOARD_LCD_GAP_Y != 0)
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, BOARD_LCD_GAP_X, BOARD_LCD_GAP_Y));
#endif

    // Clear screen to black before turning on backlight
    {
        size_t clear_sz = LCD_H_RES * LVGL_BUF_LINES * sizeof(uint16_t);
        void *clear_buf = heap_caps_calloc(1, clear_sz, MALLOC_CAP_DMA);
        configASSERT(clear_buf);
        for (int y = 0; y < LCD_V_RES; y += LVGL_BUF_LINES) {
            int h = (y + LVGL_BUF_LINES <= LCD_V_RES) ? LVGL_BUF_LINES : (LCD_V_RES - y);
            esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_H_RES, y + h, clear_buf);
        }
        // Wait for all queued SPI DMA transfers to complete before freeing
        vTaskDelay(pdMS_TO_TICKS(100));
        free(clear_buf);
    }

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, config_store_get_brightness());
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);

    // LVGL init
    lv_init();

    lv_display_t *display = lv_display_create(LCD_H_RES, LCD_V_RES);
    if (!display) {
        ESP_LOGE(TAG, "lv_display_create failed — out of memory");
        abort();
    }

    // DMA buffers
    size_t buf_sz = LCD_H_RES * LVGL_BUF_LINES * sizeof(lv_color16_t);
    void *buf1 = heap_caps_malloc(buf_sz, MALLOC_CAP_DMA);
    void *buf2 = heap_caps_malloc(buf_sz, MALLOC_CAP_DMA);
    configASSERT(buf1 && buf2);

    lv_display_set_buffers(display, buf1, buf2, buf_sz,
                            LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_user_data(display, s_panel);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, lvgl_flush_cb);
    s_lv_display = display;

    // DMA done -> flush ready callback
    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = notify_lvgl_flush_ready,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io_handle, &cbs, display));

    // LVGL tick timer
    const esp_timer_create_args_t tick_args = {
        .callback = &lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, LVGL_TICK_MS * 1000));

    ESP_LOGI(TAG, "Display initialized: %dx%d landscape", LCD_H_RES, LCD_V_RES);
    return display;
}

void display_set_brightness(uint8_t duty)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    ESP_LOGI(TAG, "Brightness set to %u", duty);
}

void display_set_flipped(bool flipped)
{
    if (!s_panel) return;

    /* 180° rotation in landscape = XOR both mirror flags against the board's
     * native orientation. swap_xy stays as-is (that's the portrait↔landscape
     * axis swap which isn't what we're toggling). */
    bool mx = BOARD_LCD_MIRROR_X ? !flipped : flipped;
    bool my = BOARD_LCD_MIRROR_Y ? !flipped : flipped;
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, mx, my));
    ESP_LOGI(TAG, "Display flipped=%u (mirror_x=%u mirror_y=%u)",
             (unsigned)flipped, (unsigned)mx, (unsigned)my);

    /* Mirror flips the CASET/RASET mapping for *future* pixel writes. Anything
     * already in panel RAM (static scene layers — sky, ground) stays put and
     * is scanned out in its old orientation, so only dynamic/redrawn regions
     * appear flipped until we force every widget to re-render. Invalidating
     * the root screen marks every pixel dirty; lv_refr_now drives the full
     * redraw synchronously so the user sees one clean transition rather than
     * a several-frame tear while LVGL's partial-refresh chews through the
     * screen in tiles. Only runs after LVGL is up — the boot-time init caller
     * skips this branch because the display clear-to-black + first scene
     * render that follow already cover the full screen in the new mapping. */
    if (s_lv_display) {
        lv_obj_t *scr = lv_display_get_screen_active(s_lv_display);
        if (scr) {
            lv_obj_invalidate(scr);
            lv_refr_now(s_lv_display);
        }
    }
}
