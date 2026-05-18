// firmware/main/ui.c
#include "ui.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

#define ROWS         STATS_MAX_PROVIDERS
#define ROW_Y0       46
#define ROW_H        26

typedef enum { UI_PROVISION, UI_STATS } ui_mode_t;

// Shared state — written by any task under s_mtx, consumed only by ui_task.
static SemaphoreHandle_t s_mtx;
static struct {
    ui_mode_t mode;
    char ssid[33], pass[64];
    char status[64];
    stats_t stats;
    int64_t fetched_ms;
    bool dirty;
} st;

// Widgets (created once, mutated only on ui_task)
static lv_obj_t *scr, *title, *status, *prov_box;
static lv_obj_t *row_id[ROWS], *row_bar[ROWS], *row_val[ROWS];

static lv_color_t pct_color(float p)   // green -> amber -> red
{
    if (p >= 90) return lv_color_hex(0xe5484d);
    if (p >= 60) return lv_color_hex(0xf5a623);
    return lv_color_hex(0x30c14e);
}

static void build_widgets(void)
{
    scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0b0b0b), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    title = lv_label_create(scr);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_label_set_text(title, "CODEXBAR");
    lv_obj_set_pos(title, 8, 6);

    status = lv_label_create(scr);
    lv_obj_set_style_text_color(status, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(status, &lv_font_montserrat_12, 0);
    lv_label_set_text(status, "starting…");
    lv_obj_set_pos(status, 8, 28);

    for (int i = 0; i < ROWS; i++) {
        int y = ROW_Y0 + i * ROW_H;
        row_id[i] = lv_label_create(scr);
        lv_obj_set_style_text_color(row_id[i], lv_color_hex(0xe8eaed), 0);
        lv_obj_set_style_text_font(row_id[i], &lv_font_montserrat_14, 0);
        lv_obj_set_pos(row_id[i], 8, y);
        lv_obj_set_width(row_id[i], 86);

        row_bar[i] = lv_bar_create(scr);
        lv_obj_set_size(row_bar[i], 160, 14);
        lv_obj_set_pos(row_bar[i], 100, y + 2);
        lv_bar_set_range(row_bar[i], 0, 100);
        lv_obj_set_style_bg_color(row_bar[i], lv_color_hex(0x222428), 0);
        lv_obj_set_style_bg_color(row_bar[i], lv_color_hex(0x30c14e), LV_PART_INDICATOR);

        row_val[i] = lv_label_create(scr);
        lv_obj_set_style_text_color(row_val[i], lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(row_val[i], &lv_font_montserrat_14, 0);
        lv_obj_set_pos(row_val[i], 268, y);

        lv_obj_add_flag(row_id[i],  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(row_bar[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(row_val[i], LV_OBJ_FLAG_HIDDEN);
    }

    prov_box = lv_label_create(scr);
    lv_obj_set_style_text_color(prov_box, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(prov_box, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(prov_box, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(prov_box, 300);
    lv_obj_set_pos(prov_box, 10, 50);
    lv_obj_add_flag(prov_box, LV_OBJ_FLAG_HIDDEN);
}

static void render(void)   // ui_task only
{
    if (st.mode == UI_PROVISION) {
        for (int i = 0; i < ROWS; i++) {
            lv_obj_add_flag(row_id[i],  LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(row_bar[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(row_val[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_clear_flag(prov_box, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(title, "SETUP");
        lv_label_set_text(status, "Join this WiFi, then open 192.168.4.1");
        lv_label_set_text_fmt(prov_box,
            "WiFi:  %s\nPass:  %s\n\nThen enter your home WiFi +\nUpstash URL + read-only token.",
            st.ssid, st.pass);
        return;
    }

    lv_obj_add_flag(prov_box, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(title, "CODEXBAR");
    // Audit State§HIGH/MED: compose the age suffix HERE from st.fetched_ms
    // (a local buffer — never mutate shared st.status). This removes the old
    // save/restore-under-mutex hack and the freshness gap where the counter
    // froze ~10 s after a fetch.
    if (st.fetched_ms > 0) {
        int age = (int)((esp_timer_get_time() / 1000 - st.fetched_ms) / 1000);
        if (age < 0) age = 0;
        char line[96];   // st.status(≤63) + " · updated <int>s ago"
        snprintf(line, sizeof line, "%s · updated %ds ago", st.status, age);
        lv_label_set_text(status, line);
    } else {
        lv_label_set_text(status, st.status);
    }

    for (int i = 0; i < ROWS; i++) {
        if (i >= st.stats.n) {
            lv_obj_add_flag(row_id[i],  LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(row_bar[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(row_val[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const stats_provider_t *p = &st.stats.p[i];
        lv_obj_clear_flag(row_id[i],  LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(row_val[i], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(row_id[i], p->id);

        if (!p->ok || !p->has_p) {
            lv_obj_add_flag(row_bar[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_color(row_val[i], lv_color_hex(0x6b7075), 0);
            lv_label_set_text(row_val[i], "off");
        } else {
            int v = (int)(p->p + 0.5f);
            if (v < 0) v = 0; else if (v > 100) v = 100;
            lv_obj_clear_flag(row_bar[i], LV_OBJ_FLAG_HIDDEN);
            lv_bar_set_value(row_bar[i], v, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(row_bar[i], pct_color(p->p), LV_PART_INDICATOR);
            lv_obj_set_style_text_color(row_val[i], lv_color_hex(0xffffff), 0);
            if (p->p < 1.0f && p->p > 0.0f)
                lv_label_set_text_fmt(row_val[i], "%.1f%%", p->p);
            else
                lv_label_set_text_fmt(row_val[i], "%d%%", v);
        }
    }
}

static void ui_task(void *arg)
{
    (void)arg;
    build_widgets();
    int64_t next_age = 0;
    while (1) {
        // Audit State§HIGH: block briefly for the mutex rather than skipping
        // the render entirely (take(0)) under setter contention.
        if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5)) == pdTRUE) {
            int64_t now = esp_timer_get_time() / 1000;
            // Re-render every 10 s in stats mode so the "updated Ns ago"
            // counter ticks even without new data. render() recomputes the
            // age from st.fetched_ms, so this is always accurate (no gap).
            if (st.mode == UI_STATS && st.fetched_ms > 0 && now >= next_age) {
                next_age = now + 10000;
                st.dirty = true;
            }
            if (st.dirty) { render(); st.dirty = false; }
            xSemaphoreGive(s_mtx);
        }
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void ui_start(void)
{
    s_mtx = xSemaphoreCreateMutex();
    st.mode = UI_STATS;
    strlcpy(st.status, "starting…", sizeof st.status);
    st.dirty = true;
    xTaskCreate(ui_task, "ui", 8192, NULL, 5, NULL);
}

static void mark(void) { st.dirty = true; }

void ui_set_provisioning(const char *ssid, const char *pass)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    st.mode = UI_PROVISION;
    strlcpy(st.ssid, ssid ? ssid : "", sizeof st.ssid);
    strlcpy(st.pass, pass ? pass : "", sizeof st.pass);
    mark();
    xSemaphoreGive(s_mtx);
}

void ui_set_status(const char *text)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    strlcpy(st.status, text ? text : "", sizeof st.status);
    mark();
    xSemaphoreGive(s_mtx);
}

void ui_set_stats(const stats_t *s, int64_t fetched_uptime_ms)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    st.mode = UI_STATS;
    if (s) st.stats = *s;
    st.fetched_ms = fetched_uptime_ms;
    mark();
    xSemaphoreGive(s_mtx);
}
