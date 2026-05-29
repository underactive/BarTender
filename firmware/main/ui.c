// firmware/main/ui.c
#include "ui.h"
#include "provider_icons.h"
#include "provider_colors.h"
#include "led.h"
#include "lvgl.h"
#include "display.h"
#include "config_store.h"
extern const lv_font_t font_lemonmilk_48;
extern const lv_font_t font_lemonmilk_24;
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define ROWS         STATS_MAX_PROVIDERS
#define ROW_Y0       20
#define ROW_H        48          // icon column + name line + bar/% line
#define ROW_ICON_PX  32          // matches scripts/gen-provider-icons.py
#define ROW_TXT_X    48          // name/bar start (right of the icon column)

#define NAV_HIST_PTS STATS_HIST_MAX   // chart points == payload schema cap,
                                      // NOT a UI choice (keep them equal)

typedef enum { UI_PROVISION, UI_STATS } ui_mode_t;
// Sub-state of UI_STATS: the scrollable summary list, or a per-provider page
// (Cost/Limit, toggled by tap; entered by tapping a summary row).
typedef enum { NAV_SUMMARY, NAV_PAGE } nav_level_t;
typedef enum { CARD_COST, CARD_LIMITS } card_kind_t;

#define SCREENSAVER_IDLE_MS     (5LL * 60LL * 1000LL)
#define SCREENSAVER_ACTIVE_MS   (8LL * 60LL * 60LL * 1000LL)
#define SCREENSAVER_PAGE_MS     (15LL * 1000LL)
#define SCREENSAVER_FADE_MS     700LL
#define SCREENSAVER_DIM_DUTY    8

typedef struct {
    char id[STATS_ID_MAX];
    bool seen;
    bool has_sig;
    uint32_t sig;
    int64_t last_change_ms;
} saver_activity_t;

// Shared state — written by any task under s_mtx, consumed only by ui_task.
static SemaphoreHandle_t s_mtx;
static SemaphoreHandle_t s_shot_sem;  // binary; given by ui_task after snapshot render
static struct {
    ui_mode_t mode;
    char ssid[33], pass[64];
    bool prov_wifi_only;          // UI_PROVISION: add-network vs first-boot copy
    char status[64];
    stats_t stats;
    int64_t fetched_ms;
    bool dirty;
    bool shot_req;                // screenshot: signal ui_task to force full re-render
    // Navigation (mutated by ui_handle_input under s_mtx; read by render).
    nav_level_t nav_level;
    int         nav_provider;   // index into stats.p[] of the chosen provider
    char        nav_id[STATS_ID_MAX];  // its id — re-resolved each render so a
                                // refresh that REORDERS providers can't
                                // silently swap which one the page shows
    card_kind_t nav_card;       // which page is showing in NAV_PAGE
    int         scroll;         // NAV_SUMMARY: index of the top visible row
    // Screensaver state: activity tracking, idle entry/exit, fade/dim state.
    saver_activity_t activity[STATS_MAX_PROVIDERS];
    bool saver_active, saver_dim_only;
    nav_level_t saved_nav_level;
    int saved_nav_provider;
    char saved_nav_id[STATS_ID_MAX];
    card_kind_t saved_nav_card;
    int saved_scroll;
    char saver_id[STATS_ID_MAX];
    card_kind_t saver_card;
    int64_t last_input_ms, saver_next_cycle_ms;
    uint8_t saver_brightness, saver_target_brightness;
    int64_t saver_fade_start_ms, saver_fade_end_ms;
    bool saver_transitioning;
    char saver_next_id[STATS_ID_MAX];
    card_kind_t saver_next_card;
    bool saver_next_dim_only;
    bool saver_show_summary;
    bool saver_next_show_summary;
} st;

// Widgets (created once, mutated only on ui_task)
static lv_obj_t *scr, *title, *status, *prov_box;
static lv_obj_t *row_id[ROWS], *row_bar[ROWS], *row_val[ROWS], *row_icon[ROWS], *row_bar_w[ROWS];

// Cost card
static lv_obj_t *cost_card, *cost_hdr, *cost_logo, *cost_big, *cost_lbl, *cost_tok, *cost_tok_unit,
                *cost_30, *cost_bar, *cost_bar_lbl, *cost_na, *cost_cap;
static lv_obj_t      *cost_chart;
static lv_chart_series_t *cost_ser;
// OpenRouter balance layout (balance hero + today/week secondary rows)
static lv_obj_t *cost_or_lbl, *cost_or_row1, *cost_or_row2;

// Usage-Limits card
static lv_obj_t *lim_card, *lim_hdr, *lim_logo,
                *lim_s_lbl, *lim_s_big, *lim_s_bar, *lim_s_rst,
                *lim_a_lbl, *lim_a_big, *lim_a_bar, *lim_a_rst,
                *lim_w_lbl, *lim_w_big, *lim_w_bar, *lim_w_rst,
                *lim_x_lbl, *lim_x_val, *lim_x_bar, *lim_cap;
static lv_obj_t      *lim_chart;
static lv_chart_series_t *lim_ser;



// Animation state — ui_task only, no mutex needed
static nav_level_t s_prev_nav_level    = NAV_SUMMARY;
static int         s_prev_nav_provider = -1;
static card_kind_t s_prev_nav_card     = CARD_COST;
static int         s_prev_row_bar[ROWS];   // last fill value per summary slot; -1 = unset

static int s_scr_w = 240;   // cached screen width (set in build_widgets);
static int s_scr_h = 320;   // cached screen height (set in build_widgets);
                            // read by ui_handle_input -> NO LVGL call off-task

typedef struct { int x, y, w, h; } ui_rect_t;
typedef struct { ui_rect_t content; int cell_w; int cell_h; } ui_page_grid_t;

#define UI_GRID_COLS     2
#define UI_GRID_ROWS     8
#define UI_CHROME_TOP    20
#define UI_CHROME_BOTTOM 16
#define UI_SUMMARY_GAP   8
#define UI_GRID_COLOR    0x8da4c0
#define UI_GRID_OPA      LV_OPA_90

static lv_obj_t *grid_h[UI_GRID_ROWS + 1];
static lv_obj_t *grid_v[UI_GRID_COLS - 1];
static lv_point_precise_t grid_h_pts[UI_GRID_ROWS + 1][2];
static lv_point_precise_t grid_v_pts[UI_GRID_COLS - 1][2];

static ui_page_grid_t ui_grid_from_height(int screen_w, int screen_h)
{
    const int content_h = screen_h - UI_CHROME_TOP - UI_CHROME_BOTTOM;
    return (ui_page_grid_t){
        .content = { 0, UI_CHROME_TOP, screen_w, content_h },
        .cell_w = screen_w / UI_GRID_COLS,
        .cell_h = content_h / UI_GRID_ROWS,
    };
}

static ui_rect_t ui_grid_span(const ui_page_grid_t *g, int col, int row, int cols, int rows)
{
    return (ui_rect_t){
        .x = g->content.x + col * g->cell_w,
        .y = g->content.y + row * g->cell_h,
        .w = cols * g->cell_w,
        .h = rows * g->cell_h,
    };
}

static void ui_update_grid_overlay(const ui_page_grid_t *g)
{
    for (int i = 0; i <= UI_GRID_ROWS; i++) {
        lv_obj_t *line = grid_h[i];
        if (!line) continue;
        grid_h_pts[i][0] = (lv_point_precise_t){ g->content.x, g->content.y + i * g->cell_h };
        grid_h_pts[i][1] = (lv_point_precise_t){ g->content.x + g->content.w, g->content.y + i * g->cell_h };
        lv_line_set_points(line, grid_h_pts[i], 2);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(line);
    }
    for (int i = 0; i < UI_GRID_COLS - 1; i++) {
        lv_obj_t *line = grid_v[i];
        if (!line) continue;
        const int x = g->content.x + (i + 1) * g->cell_w;
        grid_v_pts[i][0] = (lv_point_precise_t){ x, g->content.y };
        grid_v_pts[i][1] = (lv_point_precise_t){ x, g->content.y + g->content.h };
        lv_line_set_points(line, grid_v_pts[i], 2);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(line);
    }
}

static int summary_vis_rows_from_grid(const ui_page_grid_t *g)
{
    int rows = g->content.h / g->cell_h;
    if (rows < 1) rows = 1;
    if (rows > ROWS) rows = ROWS;
    return rows;
}

// Forward declarations for functions defined later but used by screensaver helpers.
static bool is_hidden_provider(const char *id);
static void clamp_scroll(void);

static bool provider_has_limits_card(const stats_provider_t *p)
{
    return p->has_p || p->has_s || p->has_t || p->pct_hist_n > 0 ||
           (p->has_cost && p->extra_limit_c > 0);
}

static int pct_tenths(bool has, float v)
{
    if (!has) return -1;
    int t = (int)(v * 10.0f + 0.5f);
    if (t < 0) t = 0; else if (t > 1000) t = 1000;
    return t;
}

static uint32_t hash_mix_u32(uint32_t h, uint32_t v)
{
    h ^= v + 0x9e3779b9U + (h << 6) + (h >> 2);
    return h;
}

static uint32_t provider_metric_sig(const stats_provider_t *p)
{
    uint32_t h = 2166136261U;
    h = hash_mix_u32(h, p->ok ? 1U : 0U);
    h = hash_mix_u32(h, (uint32_t)pct_tenths(p->has_p, p->p));
    h = hash_mix_u32(h, (uint32_t)pct_tenths(p->has_s, p->s));
    h = hash_mix_u32(h, (uint32_t)pct_tenths(p->has_t, p->t));
    h = hash_mix_u32(h, p->has_cost ? 1U : 0U);
    if (p->has_cost) {
        h = hash_mix_u32(h, (uint32_t)p->cost_today_c);
        h = hash_mix_u32(h, (uint32_t)p->cost_month_c);
        h = hash_mix_u32(h, (uint32_t)p->tok_today);
        h = hash_mix_u32(h, (uint32_t)(p->tok_today >> 32));
        h = hash_mix_u32(h, (uint32_t)p->tok_month);
        h = hash_mix_u32(h, (uint32_t)(p->tok_month >> 32));
        h = hash_mix_u32(h, (uint32_t)p->extra_used_c);
        h = hash_mix_u32(h, (uint32_t)p->extra_limit_c);
        h = hash_mix_u32(h, (uint32_t)p->cost_week_c);
        h = hash_mix_u32(h, (uint32_t)p->credits_remaining_c);
        h = hash_mix_u32(h, (uint32_t)p->credits_limit_c);
        h = hash_mix_u32(h, (uint32_t)p->hist_n);
        for (int i = 0; i < p->hist_n && i < STATS_HIST_MAX; i++)
            h = hash_mix_u32(h, (uint32_t)p->hist[i]);
    }
    h = hash_mix_u32(h, (uint32_t)p->pct_hist_n);
    for (int i = 0; i < p->pct_hist_n && i < STATS_PCT_HIST_MAX; i++)
        h = hash_mix_u32(h, p->pct_hist[i]);
    h = hash_mix_u32(h, p->has_lm ? 1U : 0U);
    if (p->has_lm) {
        h = hash_mix_u32(h, (uint32_t)p->lm_req_today);
        h = hash_mix_u32(h, (uint32_t)p->lm_tok_today);
        h = hash_mix_u32(h, (uint32_t)(p->lm_tok_today >> 32));
        h = hash_mix_u32(h, (uint32_t)p->lm_req_month_max);
        h = hash_mix_u32(h, (uint32_t)p->lm_tok_month_max);
        h = hash_mix_u32(h, (uint32_t)(p->lm_tok_month_max >> 32));
        h = hash_mix_u32(h, (uint32_t)p->lm_hr_n);
        for (int i = 0; i < p->lm_hr_n && i < STATS_HIST_MAX; i++)
            h = hash_mix_u32(h, (uint32_t)p->lm_hr[i]);
        h = hash_mix_u32(h, (uint32_t)p->lm_ht_n);
        for (int i = 0; i < p->lm_ht_n && i < STATS_HIST_MAX; i++)
            h = hash_mix_u32(h, (uint32_t)p->lm_ht[i]);
        h = hash_mix_u32(h, (uint32_t)p->lm_models_n);
        h = hash_mix_u32(h, (uint32_t)p->lm_week_n);
    }
    h = hash_mix_u32(h, p->has_cu ? 1U : 0U);
    if (p->has_cu) {
        h = hash_mix_u32(h, p->cu_sess_ok ? 1U : 0U);
        h = hash_mix_u32(h, (uint32_t)p->cu_tok_today);
        h = hash_mix_u32(h, (uint32_t)(p->cu_tok_today >> 32));
        h = hash_mix_u32(h, (uint32_t)p->cu_tok_month_max);
        h = hash_mix_u32(h, (uint32_t)(p->cu_tok_month_max >> 32));
        h = hash_mix_u32(h, (uint32_t)p->cu_ht_n);
        for (int i = 0; i < p->cu_ht_n && i < STATS_HIST_MAX; i++)
            h = hash_mix_u32(h, (uint32_t)p->cu_ht[i]);
    }
    return h;
}

static saver_activity_t *activity_slot(const char *id)
{
    int empty = -1;
    for (int i = 0; i < STATS_MAX_PROVIDERS; i++) {
        if (st.activity[i].seen && strcmp(st.activity[i].id, id) == 0) return &st.activity[i];
        if (!st.activity[i].seen && empty < 0) empty = i;
    }
    if (empty < 0) {
        int oldest = 0;
        for (int i = 1; i < STATS_MAX_PROVIDERS; i++)
            if (st.activity[i].last_change_ms < st.activity[oldest].last_change_ms) oldest = i;
        empty = oldest;
    }
    st.activity[empty] = (saver_activity_t){0};
    st.activity[empty].seen = true;
    strlcpy(st.activity[empty].id, id, sizeof st.activity[empty].id);
    return &st.activity[empty];
}

static void update_provider_activity_locked(const stats_t *s, int64_t now_ms)
{
    if (!s) return;
    for (int i = 0; i < s->n && i < STATS_MAX_PROVIDERS; i++) {
        const stats_provider_t *p = &s->p[i];
        if (!p->id[0] || is_hidden_provider(p->id) || !p->ok) continue;
        if (!p->has_cost && !provider_has_limits_card(p) && !p->has_lm && !p->has_cu) continue;
        uint32_t sig = provider_metric_sig(p);
        saver_activity_t *slot = activity_slot(p->id);
        if (!slot->has_sig) { slot->sig = sig; slot->has_sig = true; }
        else if (slot->sig != sig) { slot->last_change_ms = now_ms; slot->sig = sig; }
    }
}

static int find_provider_id(const char *id)
{
    for (int i = 0; id && i < st.stats.n && i < STATS_MAX_PROVIDERS; i++)
        if (strcmp(st.stats.p[i].id, id) == 0) return i;
    return -1;
}

static bool provider_card_available(const stats_provider_t *p, card_kind_t card)
{
    switch (card) {
        case CARD_COST:         return p->has_cost || p->has_lm || p->has_cu;
        case CARD_LIMITS:       return provider_has_limits_card(p) || p->has_lm;
    }
    return false;
}

static bool saver_candidate_at(int64_t now, int start, char *id, size_t id_n, card_kind_t *card)
{
    for (int step = 0; step < STATS_MAX_PROVIDERS; step++) {
        int i = (start + step) % STATS_MAX_PROVIDERS;
        saver_activity_t *a = &st.activity[i];
        if (!a->seen || !a->has_sig || a->last_change_ms <= 0) continue;
        if (now - a->last_change_ms > SCREENSAVER_ACTIVE_MS) continue;
        int pi = find_provider_id(a->id);
        if (pi < 0) continue;
        const stats_provider_t *p = &st.stats.p[pi];
        if (is_hidden_provider(p->id) || !p->ok) continue;
        if (p->has_cost || p->has_lm || p->has_cu) *card = CARD_COST;
        else if (provider_has_limits_card(p)) *card = CARD_LIMITS;
        else continue;
        strlcpy(id, a->id, id_n);
        return true;
    }
    return false;
}

static void saver_start_fade_locked(uint8_t from, uint8_t to, int64_t now)
{
    st.saver_brightness = from;
    st.saver_target_brightness = to;
    st.saver_fade_start_ms = now;
    st.saver_fade_end_ms = now + SCREENSAVER_FADE_MS;
    display_set_brightness_silent(from);
}

static void saver_step_fade_locked(int64_t now)
{
    if (st.saver_fade_end_ms > 0) {
        if (now >= st.saver_fade_end_ms) {
            // Fade completed — snap to target and handle page transition.
            display_set_brightness_silent(st.saver_target_brightness);
            st.saver_brightness = st.saver_target_brightness;
            st.saver_fade_end_ms = 0;
            // If a page transition was waiting for fade-to-dim, apply it now.
            if (st.saver_transitioning) {
                if (st.saver_next_show_summary) {
                    st.saver_show_summary = true;
                    st.nav_level = NAV_SUMMARY;
                    st.scroll = 0;
                } else {
                    st.saver_show_summary = false;
                    strlcpy(st.saver_id, st.saver_next_id, sizeof st.saver_id);
                    st.saver_card = st.saver_next_card;
                    st.saver_dim_only = st.saver_next_dim_only;
                    if (!st.saver_dim_only) {
                        int pi = find_provider_id(st.saver_id);
                        if (pi >= 0) {
                            st.nav_provider = pi;
                            strlcpy(st.nav_id, st.saver_id, sizeof st.nav_id);
                            st.nav_card = st.saver_card;
                            st.nav_level = NAV_PAGE;
                        }
                    }
                }
                st.dirty = true;
                st.saver_transitioning = false;
                st.saver_next_cycle_ms = now + SCREENSAVER_PAGE_MS;
                // Fade back up from dim to configured brightness.
                if (!st.saver_dim_only)
                    saver_start_fade_locked(SCREENSAVER_DIM_DUTY, config_store_get_brightness(), now);
            }
        } else {
            int64_t dur = st.saver_fade_end_ms - st.saver_fade_start_ms;
            int64_t el = now - st.saver_fade_start_ms;
            int duty = st.saver_brightness + (int)(((int)st.saver_target_brightness - (int)st.saver_brightness) * el / dur);
            if (duty < 0) duty = 0; else if (duty > 255) duty = 255;
            display_set_brightness_silent((uint8_t)duty);
        }
    }
}

static void saver_enter_locked(int64_t now)
{
    if (st.saver_active || st.mode != UI_STATS) return;
    st.saved_nav_level = st.nav_level;
    st.saved_nav_provider = st.nav_provider;
    strlcpy(st.saved_nav_id, st.nav_id, sizeof st.saved_nav_id);
    st.saved_nav_card = st.nav_card;
    st.saved_scroll = st.scroll;
    st.saver_active = true;
    st.saver_show_summary = false;
    st.saver_dim_only = !saver_candidate_at(now, 0, st.saver_id, sizeof st.saver_id, &st.saver_card);
    st.saver_next_cycle_ms = now + SCREENSAVER_PAGE_MS;
    saver_start_fade_locked(config_store_get_brightness(), st.saver_dim_only ? SCREENSAVER_DIM_DUTY : config_store_get_brightness(), now);
    if (!st.saver_dim_only) {
        int pi = find_provider_id(st.saver_id);
        if (pi >= 0) { st.nav_level = NAV_PAGE; st.nav_provider = pi; strlcpy(st.nav_id, st.saver_id, sizeof st.nav_id); st.nav_card = st.saver_card; }
    }
    st.dirty = true;
}

static void saver_advance_locked(int64_t now)
{
    if (!st.saver_active || st.saver_dim_only || now < st.saver_next_cycle_ms) return;
    if (st.saver_transitioning) return;  // mid-transition, wait for fade to complete

    if (st.saver_show_summary) {
        // Summary was showing — find first active provider to restart the cycle.
        if (!saver_candidate_at(now, 0, st.saver_next_id, sizeof st.saver_next_id, &st.saver_next_card)) {
            st.saver_next_dim_only = true;
        } else {
            st.saver_next_dim_only = false;
        }
        st.saver_next_show_summary = false;
    } else {
        int pi = find_provider_id(st.saver_id);
        if (pi >= 0 && strcmp(st.stats.p[pi].id, "lmstudio") == 0) {
            // LM Studio: 2-card cycle
            int next_card = ((int)st.saver_card + 1) % 2;
            if (next_card == 0 && st.saver_card == CARD_LIMITS) {
                // Wrapped around — check next provider or summary
                int start = 0;
                for (int i = 0; i < STATS_MAX_PROVIDERS; i++)
                    if (st.activity[i].seen && strcmp(st.activity[i].id, st.saver_id) == 0) { start = i + 1; break; }
                if (!saver_candidate_at(now, start, st.saver_next_id, sizeof st.saver_next_id, &st.saver_next_card)) {
                    st.saver_next_dim_only = false;
                    st.saver_next_show_summary = true;
                } else {
                    st.saver_next_dim_only = false;
                    st.saver_next_show_summary = false;
                }
                goto advance_done;
            }
            strlcpy(st.saver_next_id, st.saver_id, sizeof st.saver_next_id);
            st.saver_next_card = (card_kind_t)next_card;
            st.saver_next_dim_only = false;
            st.saver_next_show_summary = false;
        } else if (pi >= 0 && st.saver_card == CARD_COST && provider_card_available(&st.stats.p[pi], CARD_LIMITS)) {
            // Cycle Cost -> Limits on the same provider.
            strlcpy(st.saver_next_id, st.saver_id, sizeof st.saver_next_id);
            st.saver_next_card = CARD_LIMITS;
            st.saver_next_dim_only = false;
            st.saver_next_show_summary = false;
        } else {
            int start = 0;
            for (int i = 0; i < STATS_MAX_PROVIDERS; i++)
                if (st.activity[i].seen && strcmp(st.activity[i].id, st.saver_id) == 0) { start = i + 1; break; }
            if (!saver_candidate_at(now, start, st.saver_next_id, sizeof st.saver_next_id, &st.saver_next_card)) {
                // No more active providers — show summary, then wrap around.
                st.saver_next_dim_only = false;
                st.saver_next_show_summary = true;
            } else {
                st.saver_next_dim_only = false;
                st.saver_next_show_summary = false;
            }
        }
    }
    // Start transition: fade to dim, apply page when fade completes.
advance_done:
    st.saver_transitioning = true;
    saver_start_fade_locked(st.saver_brightness, SCREENSAVER_DIM_DUTY, now);
}

static void saver_exit_locked(int64_t now)
{
    if (!st.saver_active) return;
    st.saver_active = false;
    st.saver_dim_only = false;
    st.saver_transitioning = false;
    st.saver_show_summary = false;
    st.saver_next_show_summary = false;
    st.nav_level = st.saved_nav_level;
    st.nav_provider = st.saved_nav_provider;
    strlcpy(st.nav_id, st.saved_nav_id, sizeof st.nav_id);
    st.nav_card = st.saved_nav_card;
    st.scroll = st.saved_scroll;
    saver_start_fade_locked(st.saver_brightness, config_store_get_brightness(), now);
    clamp_scroll();
    st.dirty = true;
}

static lv_color_t pct_color(float p)   // green -> amber -> red
{
    if (p >= 90) return lv_color_hex(0xe5484d);
    if (p >= 60) return lv_color_hex(0xf5a623);
    return lv_color_hex(0x30c14e);
}

// Per-provider accent = the brand color CodexBar itself uses, mirrored
// verbatim from WidgetColors.color(for:) in CodexBar
// (Sources/CodexBarWidget/CodexBarWidgetViews.swift). The `id` keys are the
// UsageProvider enum raw values (= the `provider` field in our payload).
// Unknown ids fall back to the pct_color() usage ramp via bar_color().
// NOTE: a few CodexBar colors are near-black (ollama/synthetic/manus/
// commandcode); on this dark UI they read as a faint bar — kept faithful to
// CodexBar by intent (see docs). The user's live providers (claude/codex/
// cursor/openrouter) are all high-contrast.
static bool prov_accent(const char *id, lv_color_t *out)
{
    if (!id) return false;
    for (unsigned i = 0; i < PROV_COLORS_N; i++)
        if (strcmp(id, PROV_COLORS[i].id) == 0) {
            *out = lv_color_hex(PROV_COLORS[i].hex);
            return true;
        }
    return false;
}

// Providers hidden from the summary page. Add/remove IDs here to toggle
// visibility — no structural changes needed. (All known providers live in
// provider_icons.c; this is a flat list, not a bitmask.)
#define HIDDEN_PROVIDERS "ollama", "opencode", "opencodego"
static bool is_hidden_provider(const char *id)
{
    const char *h[] = { HIDDEN_PROVIDERS };
    for (size_t i = 0; i < sizeof(h) / sizeof(h[0]); i++)
        if (strcmp(id, h[i]) == 0) return true;
    return false;
}

// Progress-bar indicator color: the provider's theme accent if it has one,
// else the green/amber/red usage ramp.
static lv_color_t bar_color(const stats_provider_t *p, float v)
{
    lv_color_t c;
    if (prov_accent(p->id, &c)) return c;
    return pct_color(v);
}

// Bar fill direction. true => 0% draws FULL, 100% draws EMPTY (bars read as
// "headroom remaining"). Flip this default to change it globally; or call
// ui_set_bar_invert() at runtime (portal/NVS wiring later). Color is keyed on
// the true usage % elsewhere, so this affects fill only — never the color.
#define UI_BAR_INVERT_DEFAULT  false
static bool s_bar_invert = UI_BAR_INVERT_DEFAULT;

// Map a real 0..100 usage % to the bar's fill value, honoring the flag.
// Only call with an actual percentage — "no data" stays a literal 0 (empty).
static int bar_fill(int pct)
{
    if (pct < 0) pct = 0; else if (pct > 100) pct = 100;
    return s_bar_invert ? 100 - pct : pct;
}

// ---- scrollable-summary geometry (pure reads of st + cached s_scr_h; safe
// to call from ui_handle_input off ui_task — no LVGL, mutate only st.scroll
// under s_mtx, same discipline as the rest of the nav state) ----

// How many provider rows fit on screen below the title/status band.
static int summary_vis_rows(void)
{
    const ui_page_grid_t g = ui_grid_from_height(s_scr_w, s_scr_h);
    return summary_vis_rows_from_grid(&g);
}

// Summary rows are compacted over visible providers: hidden providers must not
// consume scroll slots or tap targets, otherwise later visible providers (Pi)
// can be pushed below blank rows.
static int summary_visible_count(void)
{
    int n = 0;
    for (int i = 0; i < st.stats.n && i < STATS_MAX_PROVIDERS; i++)
        if (!is_hidden_provider(st.stats.p[i].id)) n++;
    return n;
}

// Map a compact visible-list index back to the underlying stats.p[] index.
static int summary_provider_at(int visible_idx)
{
    if (visible_idx < 0) return -1;
    int seen = 0;
    for (int i = 0; i < st.stats.n && i < STATS_MAX_PROVIDERS; i++) {
        if (is_hidden_provider(st.stats.p[i].id)) continue;
        if (seen == visible_idx) return i;
        seen++;
    }
    return -1;
}

// Pin st.scroll into [0, max(0, visible_count - visible_rows)].
static void clamp_scroll(void)
{
    int max = summary_visible_count() - summary_vis_rows();
    if (max < 0)          max = 0;
    if (st.scroll > max)  st.scroll = max;
    if (st.scroll < 0)    st.scroll = 0;
}

// Tap y -> provider index (accounting for compact visible-list scroll), or -1
// on a miss / the inter-row gap. MUST mirror summary render geometry.
static int summary_hit_test(int y)
{
    const ui_page_grid_t g = ui_grid_from_height(s_scr_w, s_scr_h);
    if (y < g.content.y || y >= g.content.y + g.content.h) return -1;
    const int slot = (y - g.content.y) / g.cell_h;
    if (slot < 0 || slot >= summary_vis_rows_from_grid(&g)) return -1;
    if ((y - g.content.y) % g.cell_h > g.cell_h - UI_SUMMARY_GAP) return -1;
    return summary_provider_at(st.scroll + slot);
}

// tokens -> "123.2M" / "45.6K" / "789" using ONLY integer math (LVGL/newlib
// nano printf has no %lld and no float; everything here fits int32 after the
// divide: 30-day counts are < ~2e9 so /1000 or /1e6 is safe).
static void fmt_tokens(char *buf, size_t n, int64_t t)
{
    if (t < 0) t = 0;
    if (t >= 1000000000LL) {
        int cb = (int)((t + 5000000LL) / 10000000LL);  // centibillions, rounded
        snprintf(buf, n, "%d.%02dB", cb / 100, cb % 100);
    } else if (t >= 1000000) {
        int whole = (int)(t / 1000000);
        int frac  = (int)((t / 100000) % 10);
        snprintf(buf, n, "%d.%dM", whole, frac);
    } else if (t >= 1000) {
        int whole = (int)(t / 1000);
        int frac  = (int)((t / 100) % 10);
        snprintf(buf, n, "%d.%dK", whole, frac);
    } else {
        snprintf(buf, n, "%d", (int)t);
    }
}


// cents -> "$12.47" (integer only; no %f).
static void fmt_money(char *buf, size_t n, int32_t cents)
{
    if (cents < 0) cents = 0;
    // int32_t is `long` on Xtensa -> cast for %d (newlib -Werror=format).
    snprintf(buf, n, "$%d.%02d", (int)(cents / 100), (int)(cents % 100));
}


typedef struct {
    const char *title;
    const char *subtitle;
    const char *icon_id;
} ui_page_chrome_desc_t;

static void create_card_hdr(lv_obj_t *card, lv_obj_t **hdr_out, lv_obj_t **logo_out);
static void render_page_chrome(lv_obj_t *hdr, lv_obj_t *logo,
                               const ui_page_chrome_desc_t *desc);
static void bar_opa_cb(void *obj, int32_t opa);
static void update_bar_pulse(lv_obj_t *bar, float pct);

#define CURSOR_SESS_AMBER 0xF4A261

static bool cursor_sess_refresh_needed(const stats_provider_t *p)
{
    if (strcmp(p->id, "cursor") != 0 || !p->ok || !p->has_p) return false;
    if (!p->has_cu) return true;
    return !p->cu_sess_ok;
}

static void build_widgets(void)
{
    scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0b0b0b), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Width-relative layout so the same UI works in landscape (W=320) and
    // portrait (W=240). Offsets are chosen so W=320 reproduces the original
    // hardcoded landscape layout pixel-for-pixel (val_x=268, bar_w=160,
    // prov_box=300) — zero regression on the old orientation. Read from the
    // LVGL display, not BOARD_LCD_H_RES, to keep ui.c board-agnostic.
    const int W = lv_display_get_horizontal_resolution(lv_display_get_default());
    const int H = lv_display_get_vertical_resolution(lv_display_get_default());
    s_scr_w = W;
    s_scr_h = H;
    memset(s_prev_row_bar, -1, sizeof s_prev_row_bar);
    const int val_x = W - 52;          // right-anchored % column (line 2)

    title = lv_label_create(scr);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_set_pos(title, 8, 6);
    lv_label_set_text(title, "BARTENDER");

    status = lv_label_create(scr);
    lv_obj_set_style_text_color(status, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(status, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(status, 8, 4);
    lv_label_set_text(status, "starting...");

    for (int i = 0; i < ROWS; i++) {
        int y = ROW_Y0 + i * ROW_H;
        // Icon column (left): provider logo silhouette, A8, tinted per
        // render with the provider accent. Spans both text lines (32 px
        // centered in the 48 px row). src/recolor set in render().
        row_icon[i] = lv_image_create(scr);
        lv_obj_set_pos(row_icon[i], 8, y + (ROW_H - ROW_ICON_PX) / 2);
        lv_obj_set_style_image_recolor_opa(row_icon[i], LV_OPA_COVER, 0);
        lv_obj_add_flag(row_icon[i], LV_OBJ_FLAG_HIDDEN);

        row_id[i] = lv_label_create(scr);
        lv_obj_set_style_text_color(row_id[i], lv_color_hex(0xe8eaed), 0);
        lv_obj_set_style_text_font(row_id[i], &lv_font_montserrat_14, 0);
        // Line 1: provider name, right of the icon column (long ids like
        // OPENROUTER no longer clip).
        lv_obj_set_pos(row_id[i], ROW_TXT_X, y + 6);
        lv_obj_set_width(row_id[i], W - ROW_TXT_X - 8);

        // Line 2: bar (spans to just before the % column) + % (right).
        row_bar[i] = lv_bar_create(scr);
        lv_obj_set_size(row_bar[i], val_x - ROW_TXT_X - 8, 7);
        lv_obj_set_pos(row_bar[i], ROW_TXT_X, y + 30);
        lv_bar_set_range(row_bar[i], 0, 100);
        lv_obj_set_style_bg_color(row_bar[i], lv_color_hex(0x3a3a3a), 0);
        lv_obj_set_style_bg_opa(row_bar[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row_bar[i], lv_color_hex(0x30c14e), LV_PART_INDICATOR);

        row_val[i] = lv_label_create(scr);
        lv_obj_set_style_text_color(row_val[i], lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(row_val[i], &lv_font_montserrat_14, 0);
        lv_obj_set_pos(row_val[i], val_x, y + 26);

        // Weekly bar: 3 px tall, right under the session bar (y+30 + 7 + 2 = y+39).
        row_bar_w[i] = lv_bar_create(scr);
        lv_obj_set_size(row_bar_w[i], val_x - ROW_TXT_X - 8, 3);
        lv_obj_set_pos(row_bar_w[i], ROW_TXT_X, y + 39);
        lv_bar_set_range(row_bar_w[i], 0, 100);
        lv_obj_set_style_bg_color(row_bar_w[i], lv_color_hex(0x3a3a3a), 0);
        lv_obj_set_style_bg_opa(row_bar_w[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row_bar_w[i], lv_color_hex(0x30c14e), LV_PART_INDICATOR);

        lv_obj_add_flag(row_id[i],  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(row_bar[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(row_val[i], LV_OBJ_FLAG_HIDDEN);
    }

    prov_box = lv_label_create(scr);
    lv_obj_set_style_text_color(prov_box, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(prov_box, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(prov_box, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(prov_box, W - 20);
    lv_obj_set_pos(prov_box, 10, 50);
    lv_obj_add_flag(prov_box, LV_OBJ_FLAG_HIDDEN);

    const ui_page_grid_t grid = ui_grid_from_height(W, H);
    for (int i = 0; i <= UI_GRID_ROWS; i++) {
        grid_h[i] = lv_line_create(scr);
        lv_obj_clear_flag(grid_h[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(grid_h[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_line_color(grid_h[i], lv_color_hex(UI_GRID_COLOR), 0);
        lv_obj_set_style_line_opa(grid_h[i], UI_GRID_OPA, 0);
        lv_obj_set_style_line_width(grid_h[i], 1, 0);
        lv_obj_set_style_line_dash_width(grid_h[i], 1, 0);
        lv_obj_set_style_line_dash_gap(grid_h[i], 5, 0);
    }
    for (int i = 0; i < UI_GRID_COLS - 1; i++) {
        grid_v[i] = lv_line_create(scr);
        lv_obj_clear_flag(grid_v[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(grid_v[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_line_color(grid_v[i], lv_color_hex(UI_GRID_COLOR), 0);
        lv_obj_set_style_line_opa(grid_v[i], UI_GRID_OPA, 0);
        lv_obj_set_style_line_width(grid_v[i], 1, 0);
        lv_obj_set_style_line_dash_width(grid_v[i], 1, 0);
        lv_obj_set_style_line_dash_gap(grid_v[i], 5, 0);
    }
    ui_update_grid_overlay(&grid);

    // (The old swipe menu/submenu widgets were removed: the summary list is
    // now scrolled directly and provider pages are reached by tapping a row.)

    // ---- Cost card (full-screen panel; hiding the parent hides children) ----
    cost_card = lv_obj_create(scr);
    lv_obj_set_size(cost_card, W, H);
    lv_obj_set_pos(cost_card, 0, 0);
    lv_obj_set_style_bg_color(cost_card, lv_color_hex(0x0b0b0b), 0);
    lv_obj_set_style_bg_opa(cost_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cost_card, 0, 0);
    lv_obj_set_style_radius(cost_card, 0, 0);
    // The default theme pads lv_obj containers; zero it so child
    // lv_obj_set_pos() coordinates are screen-absolute (otherwise children
    // are offset and a W-24 child overflows the padded content box → "no
    // margin / runs off the right edge").
    lv_obj_set_style_pad_all(cost_card, 0, 0);
    lv_obj_clear_flag(cost_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cost_card, LV_OBJ_FLAG_HIDDEN);

    create_card_hdr(cost_card, &cost_hdr, &cost_logo);

    cost_lbl = lv_label_create(cost_card);
    lv_obj_set_style_text_color(cost_lbl, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(cost_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(cost_lbl, 12, 36);
    lv_obj_add_flag(cost_lbl, LV_OBJ_FLAG_HIDDEN);

    cost_big = lv_label_create(cost_card);
    lv_obj_set_style_text_color(cost_big, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(cost_big, &font_lemonmilk_48, 0);
    lv_obj_set_pos(cost_big, 12, 48);

    cost_tok = lv_label_create(cost_card);
    lv_obj_set_style_text_color(cost_tok, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(cost_tok, &font_lemonmilk_24, 0);
    lv_obj_set_pos(cost_tok, 12, 104);

    cost_tok_unit = lv_label_create(cost_card);
    lv_obj_set_style_text_color(cost_tok_unit, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(cost_tok_unit, &lv_font_montserrat_14, 0);
    lv_label_set_text(cost_tok_unit, "tokens");

    cost_30 = lv_label_create(cost_card);
    lv_obj_set_style_text_color(cost_30, lv_color_hex(0xe8eaed), 0);
    lv_obj_set_style_text_font(cost_30, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(cost_30, 12, H - 38);

    cost_cap = lv_label_create(cost_card);
    lv_obj_set_style_text_color(cost_cap, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(cost_cap, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(cost_cap, 12, H - 22);

    cost_chart = lv_chart_create(cost_card);
    lv_obj_set_size(cost_chart, W - 24, H - 178);
    lv_obj_set_pos(cost_chart, 12, 132);
    // BAR graph of the 30-day daily spend (CodexBar cost cache is
    // day-granular — no hourly $, so this is days, not 24h). point_count is
    // set per-render to the real history length so there are NEVER filler
    // points: LV_CHART_POINT_NONE == INT32_MAX, which a chart clamps to the
    // range max and would draw as a full-height block (the old "giant orange
    // bar" bug).
    lv_chart_set_type(cost_chart, LV_CHART_TYPE_BAR);
    lv_chart_set_div_line_count(cost_chart, 0, 0);
    lv_obj_set_style_border_width(cost_chart, 0, 0);
    lv_obj_set_style_bg_opa(cost_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(cost_chart, 2, 0);   // keep the bars off the edge
    lv_obj_set_style_pad_column(cost_chart, 2, LV_PART_MAIN);
    lv_obj_set_style_line_width(cost_chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_width(cost_chart, 0, LV_PART_INDICATOR);   // no point dots
    lv_obj_set_style_height(cost_chart, 0, LV_PART_INDICATOR);
    cost_ser = lv_chart_add_series(cost_chart, lv_color_hex(0xe06c4b),
                                   LV_CHART_AXIS_PRIMARY_Y);

    cost_bar = lv_bar_create(cost_card);
    lv_obj_set_size(cost_bar, W - 24, 6);   // half-height (was 12)
    lv_obj_set_pos(cost_bar, 12, H - 56);
    lv_bar_set_range(cost_bar, 0, 100);
    lv_obj_set_style_bg_color(cost_bar, lv_color_hex(0x3a3a3a), 0);
    lv_obj_set_style_bg_opa(cost_bar, LV_OPA_COVER, 0);

    cost_bar_lbl = lv_label_create(cost_card);
    lv_obj_set_style_text_color(cost_bar_lbl, lv_color_hex(0xe8eaed), 0);
    lv_obj_set_style_text_font(cost_bar_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(cost_bar_lbl, 12, H - 38);

    lv_obj_add_flag(cost_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(cost_bar_lbl, LV_OBJ_FLAG_HIDDEN);

    cost_na = lv_label_create(cost_card);
    lv_obj_set_style_text_color(cost_na, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(cost_na, &lv_font_montserrat_14, 0);
    lv_label_set_text(cost_na, "COST DATA NOT\nAVAILABLE YET");
    lv_obj_set_pos(cost_na, 12, 60);
    lv_obj_add_flag(cost_na, LV_OBJ_FLAG_HIDDEN);

    // OpenRouter balance layout: "BALANCE" sub-label + TODAY + THIS WEEK rows.
    // Shown instead of the Claude/Codex token+chart layout when has_balance.
    cost_or_lbl = lv_label_create(cost_card);
    lv_obj_set_style_text_color(cost_or_lbl, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(cost_or_lbl, &lv_font_montserrat_12, 0);
    lv_label_set_text(cost_or_lbl, "BALANCE");
    lv_obj_set_pos(cost_or_lbl, 12, 88);
    lv_obj_add_flag(cost_or_lbl, LV_OBJ_FLAG_HIDDEN);

    cost_or_row1 = lv_label_create(cost_card);
    lv_obj_set_style_text_color(cost_or_row1, lv_color_hex(0xe8eaed), 0);
    lv_obj_set_style_text_font(cost_or_row1, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(cost_or_row1, 12, 118);
    lv_obj_add_flag(cost_or_row1, LV_OBJ_FLAG_HIDDEN);

    cost_or_row2 = lv_label_create(cost_card);
    lv_obj_set_style_text_color(cost_or_row2, lv_color_hex(0xe8eaed), 0);
    lv_obj_set_style_text_font(cost_or_row2, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(cost_or_row2, 12, 140);
    lv_obj_add_flag(cost_or_row2, LV_OBJ_FLAG_HIDDEN);

    // ---- Usage-Limits card ----
    lim_card = lv_obj_create(scr);
    lv_obj_set_size(lim_card, W, H);
    lv_obj_set_pos(lim_card, 0, 0);
    lv_obj_set_style_bg_color(lim_card, lv_color_hex(0x0b0b0b), 0);
    lv_obj_set_style_bg_opa(lim_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(lim_card, 0, 0);
    lv_obj_set_style_radius(lim_card, 0, 0);
    lv_obj_set_style_pad_all(lim_card, 0, 0);   // see cost_card note
    lv_obj_clear_flag(lim_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(lim_card, LV_OBJ_FLAG_HIDDEN);

    create_card_hdr(lim_card, &lim_hdr, &lim_logo);

    lim_s_lbl = lv_label_create(lim_card);
    lv_obj_set_style_text_color(lim_s_lbl, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(lim_s_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lim_s_lbl, 12, 34);
    lim_s_big = lv_label_create(lim_card);
    lv_obj_set_style_text_color(lim_s_big, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lim_s_big, &font_lemonmilk_48, 0);
    lv_obj_set_pos(lim_s_big, 12, 48);
    lim_s_bar = lv_bar_create(lim_card);
    lv_obj_set_size(lim_s_bar, W - 24, 9);
    lv_obj_set_pos(lim_s_bar, 12, 104);
    lv_bar_set_range(lim_s_bar, 0, 100);
    lv_obj_set_style_bg_color(lim_s_bar, lv_color_hex(0x3a3a3a), 0);
    lv_obj_set_style_bg_opa(lim_s_bar, LV_OPA_COVER, 0);
    lim_s_rst = lv_label_create(lim_card);
    lv_obj_set_style_text_color(lim_s_rst, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(lim_s_rst, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lim_s_rst, 12, 116);

    // lim_cap (session-24h caption) permanently hidden — line removed from UI.
    lim_cap = lv_label_create(lim_card);
    lv_obj_add_flag(lim_cap, LV_OBJ_FLAG_HIDDEN);

    // Dynamic layout: compute all lower-card positions from H so the chart
    // automatically fills every available pixel between the session section
    // and the weekly section.
    const int PAD   = 10;   // gap between sections
    const int S_END = 130;  // bottom of lim_s_rst  (y=116 + font_12 ~14px)

    // Extra usage: pinned to the bottom of the screen.
    const int x_bar_y = H - 11;           // bar top (h=5 → bottom = H-6)
    const int x_lbl_y = x_bar_y - 4 - 14; // label above bar

    // Weekly: stacked immediately above the extra usage section.
    const int w_rst_y = x_lbl_y - PAD - 14;
    const int w_bar_y = w_rst_y - 4 - 5;
    const int w_big_y = w_bar_y - 4 - 26;
    const int w_lbl_y = w_big_y - 4 - 14;

    // Chart: fills the gap between the session section and the weekly section.
    const int chart_y = S_END + PAD;
    const int chart_h = (w_lbl_y - PAD) - chart_y;

    lim_chart = lv_chart_create(lim_card);
    lv_obj_set_size(lim_chart, W - 24, chart_h > 8 ? chart_h : 8);
    lv_obj_set_pos(lim_chart, 12, chart_y);
    lv_chart_set_type(lim_chart, LV_CHART_TYPE_LINE | LV_CHART_TYPE_CURVE);
    lv_chart_set_div_line_count(lim_chart, 0, 0);
    lv_obj_set_style_border_width(lim_chart, 0, 0);
    lv_obj_set_style_bg_opa(lim_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(lim_chart, 2, 0);
    lv_obj_set_style_line_width(lim_chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_width(lim_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(lim_chart, 0, LV_PART_INDICATOR);
    lim_ser = lv_chart_add_series(lim_chart, lv_color_hex(0x30c14e),
                                  LV_CHART_AXIS_PRIMARY_Y);

    // Auto section: overlaps the chart area — exactly one is visible at a time.
    // render_card() shows chart when pct_hist_n>0, Auto section otherwise.
    const int a_lbl_y = chart_y;
    const int a_big_y = a_lbl_y + 14 + 3;
    const int a_bar_y = a_big_y + 26 + 3;
    const int a_rst_y = a_bar_y + 5 + 3;
    lim_a_lbl = lv_label_create(lim_card);
    lv_obj_set_style_text_color(lim_a_lbl, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(lim_a_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lim_a_lbl, 12, a_lbl_y);
    lim_a_big = lv_label_create(lim_card);
    lv_obj_set_style_text_color(lim_a_big, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lim_a_big, &font_lemonmilk_24, 0);
    lv_obj_set_pos(lim_a_big, 12, a_big_y);
    lim_a_bar = lv_bar_create(lim_card);
    lv_obj_set_size(lim_a_bar, W - 24, 5);
    lv_obj_set_pos(lim_a_bar, 12, a_bar_y);
    lv_bar_set_range(lim_a_bar, 0, 100);
    lv_obj_set_style_bg_color(lim_a_bar, lv_color_hex(0x3a3a3a), 0);
    lv_obj_set_style_bg_opa(lim_a_bar, LV_OPA_COVER, 0);
    lim_a_rst = lv_label_create(lim_card);
    lv_obj_set_style_text_color(lim_a_rst, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(lim_a_rst, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lim_a_rst, 12, a_rst_y);
    lv_obj_add_flag(lim_a_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lim_a_big, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lim_a_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lim_a_rst, LV_OBJ_FLAG_HIDDEN);

    lim_w_lbl = lv_label_create(lim_card);
    lv_obj_set_style_text_color(lim_w_lbl, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(lim_w_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lim_w_lbl, 12, w_lbl_y);
    lim_w_big = lv_label_create(lim_card);
    lv_obj_set_style_text_color(lim_w_big, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lim_w_big, &font_lemonmilk_24, 0);
    lv_obj_set_pos(lim_w_big, 12, w_big_y);
    lim_w_bar = lv_bar_create(lim_card);
    lv_obj_set_size(lim_w_bar, W - 24, 5);
    lv_obj_set_pos(lim_w_bar, 12, w_bar_y);
    lv_bar_set_range(lim_w_bar, 0, 100);
    lv_obj_set_style_bg_color(lim_w_bar, lv_color_hex(0x3a3a3a), 0);
    lv_obj_set_style_bg_opa(lim_w_bar, LV_OPA_COVER, 0);
    lim_w_rst = lv_label_create(lim_card);
    lv_obj_set_style_text_color(lim_w_rst, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(lim_w_rst, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lim_w_rst, 12, w_rst_y);
    lv_obj_add_flag(lim_w_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lim_w_big, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lim_w_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lim_w_rst, LV_OBJ_FLAG_HIDDEN);

    lim_x_lbl = lv_label_create(lim_card);
    lv_obj_set_style_text_color(lim_x_lbl, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(lim_x_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lim_x_lbl, 12, x_lbl_y);
    lim_x_val = lv_label_create(lim_card);
    lv_obj_set_style_text_color(lim_x_val, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(lim_x_val, &lv_font_montserrat_12, 0);
    lv_obj_set_width(lim_x_val, W - 24);
    lv_obj_set_pos(lim_x_val, 12, x_lbl_y);
    lv_obj_set_style_text_align(lim_x_val, LV_TEXT_ALIGN_RIGHT, 0);
    lim_x_bar = lv_bar_create(lim_card);
    lv_obj_set_size(lim_x_bar, W - 24, 5);
    lv_obj_set_pos(lim_x_bar, 12, x_bar_y);
    lv_bar_set_range(lim_x_bar, 0, 100);
    lv_obj_set_style_bg_color(lim_x_bar, lv_color_hex(0x3a3a3a), 0);
    lv_obj_set_style_bg_opa(lim_x_bar, LV_OPA_COVER, 0);

}

// ---- navigation helpers --------------------------------------------------

static void up_id(char *dst, size_t n, const char *src)
{
    size_t j = 0;
    for (; src && src[j] && j + 1 < n; j++)
        dst[j] = (char)toupper((unsigned char)src[j]);
    dst[j] = '\0';
}

static void create_card_hdr(lv_obj_t *card, lv_obj_t **hdr_out, lv_obj_t **logo_out)
{
    *logo_out = lv_image_create(card);
    lv_obj_set_pos(*logo_out, 2, 2);
    lv_obj_set_style_image_recolor_opa(*logo_out, LV_OPA_COVER, 0);
    lv_image_set_pivot(*logo_out, 0, 0);
    lv_image_set_scale(*logo_out, 112);   // 32px * (112/256) ≈ 14px = font_14 height
    lv_obj_add_flag(*logo_out, LV_OBJ_FLAG_HIDDEN);

    *hdr_out = lv_label_create(card);
    lv_obj_set_style_text_color(*hdr_out, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(*hdr_out, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(*hdr_out, 2, 2);
}

static void render_page_chrome(lv_obj_t *hdr, lv_obj_t *logo,
                               const ui_page_chrome_desc_t *desc)
{
    if (hdr) {
        if (desc && desc->subtitle)
            lv_label_set_text_fmt(hdr, "%s  %s", desc->title, desc->subtitle);
        else if (desc)
            lv_label_set_text(hdr, desc->title);
    }
    if (!logo || !desc || !desc->icon_id) return;

    const lv_image_dsc_t *ic = provider_icon(desc->icon_id);
    if (ic) {
        lv_image_set_src(logo, ic);
        if (provider_icon_is_full_color(desc->icon_id)) {
            lv_obj_set_style_image_recolor_opa(logo, LV_OPA_TRANSP, 0);
        } else {
            lv_color_t tc;
            lv_obj_set_style_image_recolor_opa(logo, LV_OPA_COVER, 0);
            lv_obj_set_style_image_recolor(logo,
                prov_accent(desc->icon_id, &tc) ? tc : lv_color_hex(0xe8eaed), 0);
        }
        lv_obj_clear_flag(logo, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(hdr, 22, 2);
    } else {
        lv_obj_add_flag(logo, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(hdr, 2, 2);
    }
}


static void hide_cards(void)     // hide all card panels (chrome stays)
{
    update_bar_pulse(lim_s_bar, 0.0f);
    update_bar_pulse(lim_a_bar, 0.0f);
    update_bar_pulse(lim_w_bar, 0.0f);
    update_bar_pulse(lim_x_bar, 0.0f);
    lv_obj_add_flag(cost_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lim_card,  LV_OBJ_FLAG_HIDDEN);
}

static void hide_summary_chrome(void)  // hide title/status/rows before a card
{
    lv_obj_add_flag(title,    LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(status,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(prov_box, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < ROWS; i++) {
        update_bar_pulse(row_bar[i], 0.0f);
        update_bar_pulse(row_bar_w[i], 0.0f);
        lv_obj_add_flag(row_id[i],   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(row_bar[i],  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(row_val[i],  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(row_icon[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(row_bar_w[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void fmt_pct(char *buf, size_t n, bool has, float v)
{
    if (!has) { snprintf(buf, n, "--"); return; }
    // Always 1 decimal place. LVGL sprintf has no float support (CONFIG_LV_USE_FLOAT
    // unset), so use integer tenths: 45.3 -> tenths=453 -> "45.3%".
    int tenths = (int)(v * 10.0f + 0.5f);
    if (tenths < 0) tenths = 0; else if (tenths > 1000) tenths = 1000;
    snprintf(buf, n, "%d.%d%%", tenths / 10, tenths % 10);
}

// ── Animation helpers (ui_task only) ─────────────────────────────────────────

static void bar_opa_cb(void *obj, int32_t opa)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)opa, LV_PART_INDICATOR);
}

// Start a heartbeat pulse on bars ≥90 %; stop it when they fall back below.
static void update_bar_pulse(lv_obj_t *bar, float pct)
{
    bool should  = (pct >= 90.0f);
    bool running = (lv_anim_get(bar, bar_opa_cb) != NULL);
    if (should && !running) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, bar);
        lv_anim_set_exec_cb(&a, bar_opa_cb);
        lv_anim_set_values(&a, LV_OPA_40, LV_OPA_COVER);
        lv_anim_set_duration(&a, 700);
        lv_anim_set_playback_duration(&a, 700);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    } else if (!should && running) {
        lv_anim_delete(bar, bar_opa_cb);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    }
}

static void cursor_icon_opa_cb(void *obj, int32_t opa)
{
    lv_obj_set_style_image_recolor_opa((lv_obj_t *)obj, (lv_opa_t)opa, 0);
}

// Slow amber pulse on the Cursor icon when the Mac session cookie needs refresh.
static void update_cursor_sess_pulse(lv_obj_t *icon, bool needs)
{
    bool running = (lv_anim_get(icon, cursor_icon_opa_cb) != NULL);
    if (needs && !running) {
        lv_obj_set_style_image_recolor(icon, lv_color_hex(CURSOR_SESS_AMBER), 0);
        lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, icon);
        lv_anim_set_exec_cb(&a, cursor_icon_opa_cb);
        lv_anim_set_values(&a, LV_OPA_30, LV_OPA_COVER);
        lv_anim_set_duration(&a, 1200);
        lv_anim_set_playback_duration(&a, 1200);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    } else if (!needs && running) {
        lv_anim_delete(icon, cursor_icon_opa_cb);
        lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    }
}

static void count_pct_cb(void *obj, int32_t v)
{
    lv_label_set_text_fmt((lv_obj_t *)obj, "%d.%d%%", (int)(v / 10), (int)(v % 10));
}

static void count_cents_cb(void *obj, int32_t v)
{
    lv_label_set_text_fmt((lv_obj_t *)obj, "$%d.%02d", (int)(v / 100), (int)(v % 100));
}

// Animate a numeric hero label from 0 to target using the provided formatter.
static void anim_count_up(lv_obj_t *lbl, int32_t target, lv_anim_exec_xcb_t cb)
{
    lv_anim_delete(lbl, cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, lbl);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, 0, target > 0 ? target : 0);
    lv_anim_set_duration(&a, 400);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void chart_opa_cb(void *obj, int32_t opa)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)opa, 0);
}

// Fade a chart in from transparent over 600 ms on card entry.
static void anim_chart_fadein(lv_obj_t *chart)
{
    lv_anim_delete(chart, chart_opa_cb);
    lv_obj_set_style_opa(chart, LV_OPA_TRANSP, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, chart);
    lv_anim_set_exec_cb(&a, chart_opa_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, 600);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

// ─────────────────────────────────────────────────────────────────────────────

static void set_bar(lv_obj_t *bar, bool has, float v, const stats_provider_t *p)
{
    int iv = (int)(v + 0.5f);
    if (iv < 0) iv = 0; else if (iv > 100) iv = 100;
    lv_bar_set_value(bar, has ? bar_fill(iv) : 0, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar, bar_color(p, v), LV_PART_INDICATOR);
    update_bar_pulse(bar, has ? v : 0.0f);
}

static int32_t render_cost_bar_chart(lv_obj_t *chart, lv_chart_series_t *ser,
                                     const int32_t *hist, int n, lv_color_t color)
{
    int draw_n = (n < 2) ? 2 : n;
    int32_t mx = 0;
    for (int i = 0; i < n; i++)
        if (hist[i] > mx) mx = hist[i];
    int32_t chart_max = (mx > 0) ? mx : 1;

    lv_chart_set_point_count(chart, (uint32_t)draw_n);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, chart_max);
    lv_chart_set_series_color(chart, ser, color);
    for (int i = 0; i < draw_n; i++)
        lv_chart_set_value_by_id(chart, ser, i, (i < n) ? hist[i] : 0);
    lv_chart_refresh(chart);
    return mx;
}

// Extra-usage overage as a clamped 0..100 %. 0 when unknown / no limit.
// Single-sourced: used by both the Cost and Usage-Limits cards.
static int extra_pct(const stats_provider_t *p)
{
    if (!p->has_cost || p->extra_limit_c <= 0) return 0;
    int xp = (int)(((int64_t)p->extra_used_c * 100) / p->extra_limit_c);
    if (xp < 0) xp = 0; else if (xp > 100) xp = 100;
    return xp;
}

// Set a reset-time label from a timestamp string, or hide it if empty.
// Normalises both raw ("May 19 at 3:10PM") and pre-prefixed ("Resets May 19 at 3:10PM") inputs.
static void set_reset_lbl(lv_obj_t *lbl, const char *ts)
{
    if (!ts || !ts[0]) { lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN); return; }
    if (strncmp(ts, "Resets ", 7) == 0) ts += 7;
    char buf[48];
    const char *at = strstr(ts, " at ");
    if (at)
        snprintf(buf, sizeof buf, "Resets %.*s %s", (int)(at - ts), ts, at + 4);
    else
        snprintf(buf, sizeof buf, "Resets %s", ts);
    lv_label_set_text(lbl, buf);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_HIDDEN);
}

// Show cost_lbl as the caption above the hero amount (e.g. "SPEND", "TOKENS").
// Anchored at hero.y + 2 — just below the header chrome — so it always clears
// the -8-leading-trimmed number set unconditionally in render_card(). New
// providers that want a caption call this instead of hand-placing cost_lbl, so
// the position can't drift out of sync with the amount.
static void cost_hero_caption(const ui_rect_t *hero, const char *text)
{
    lv_obj_clear_flag(cost_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(cost_lbl, text);
    lv_obj_set_pos(cost_lbl, hero->x + 12, hero->y + 2);
}

static void render_card(void)   // ui_task only (renders the NAV_PAGE card)
{
    bool card_entered = (s_prev_nav_level    != NAV_PAGE        ||
                         s_prev_nav_provider != st.nav_provider ||
                         s_prev_nav_card     != st.nav_card);
    s_prev_nav_level    = NAV_PAGE;
    s_prev_nav_provider = st.nav_provider;
    s_prev_nav_card     = st.nav_card;

    hide_summary_chrome();

    const stats_provider_t *p = &st.stats.p[st.nav_provider];
    // Data-driven: any provider with a balance/credits field uses the balance layout.
    // Avoids hardcoding "openrouter" and works for any future provider with the same shape.
    bool has_balance = (p->credits_limit_c > 0 || p->credits_remaining_c > 0);
    bool is_pi = (strcmp(p->id, "pi") == 0);
    bool is_lmstudio = (strcmp(p->id, "lmstudio") == 0);
    bool is_cursor = (strcmp(p->id, "cursor") == 0);
    const ui_page_grid_t g = ui_grid_from_height(s_scr_w, s_scr_h);
    const ui_rect_t hero = ui_grid_span(&g, 0, 0, 2, 2);
    const ui_rect_t body = ui_grid_span(&g, 0, 2, 2, 4);
    const ui_rect_t footer = ui_grid_span(&g, 0, 6, 2, 2);

    ui_update_grid_overlay(&g);

    if (st.nav_card == CARD_COST) {
        lv_obj_add_flag(lim_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(cost_card, LV_OBJ_FLAG_HIDDEN);
        {
            char up[STATS_ID_MAX];
            up_id(up, sizeof up, p->id);
            render_page_chrome(cost_hdr, cost_logo, &(ui_page_chrome_desc_t){
                .title = up,
                .subtitle = "TODAY",
                .icon_id = p->id,
            });
        }

        if (!p->has_cost && !p->has_lm && !p->has_cu) {
            lv_obj_clear_flag(cost_na, LV_OBJ_FLAG_HIDDEN);
            lv_obj_t *all[] = { cost_big, cost_tok, cost_tok_unit, cost_30, cost_cap,
                                cost_chart, cost_bar, cost_bar_lbl,
                                cost_or_lbl, cost_or_row1, cost_or_row2 };
            for (unsigned i = 0; i < sizeof all / sizeof *all; i++)
                lv_obj_add_flag(all[i], LV_OBJ_FLAG_HIDDEN);
            return;
        }
        lv_obj_add_flag(cost_na, LV_OBJ_FLAG_HIDDEN);

        // lemonmilk-48 reserves ~8px of top leading inside its 49px line box,
        // which pushes the hero amount past the bottom of the 2x2 grid region.
        // Trim it for every provider so the number stays in bounds and sits at
        // a consistent height across cards. Captions above it (SPEND/TOKENS)
        // are raised to hero.y + 2 to match.
        lv_obj_set_style_pad_top(cost_big, -8, 0);

        // Secondary metric line (tokens / requests / balance) — same height on
        // every card. hero.y + 76 centers it in body-row 2 so it clears the
        // grid divider lines above (y=90) and below (y=125). cost_tok_unit is
        // aligned to it per-branch once its text width is known.
        lv_obj_set_pos(cost_tok, hero.x + 12, hero.y + 76);

        if (is_lmstudio) {
            // LM Studio TODAY: hero tokens, hero requests, 30-day bar chart, 30-day max
            lv_obj_t *hide[] = { cost_or_lbl, cost_or_row1, cost_or_row2,
                                 cost_bar, cost_bar_lbl, cost_cap };
            for (unsigned i = 0; i < sizeof hide / sizeof *hide; i++)
                lv_obj_add_flag(hide[i], LV_OBJ_FLAG_HIDDEN);
            cost_hero_caption(&hero, "TOKENS");
            lv_obj_t *show[] = { cost_big, cost_tok, cost_tok_unit, cost_30, cost_chart };
            for (unsigned i = 0; i < sizeof show / sizeof *show; i++)
                lv_obj_clear_flag(show[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(cost_big, hero.x + 12, hero.y + 28);
            lv_obj_set_pos(cost_30, footer.x + 12, footer.y + 6);
            lv_obj_set_pos(cost_cap, footer.x + 12, footer.y + 22);
            lv_obj_set_size(cost_chart, body.w - 24, body.h - 8);
            lv_obj_set_pos(cost_chart, body.x + 12, body.y + 4);
            char tk[16], rq[16], tk30[16], rq30[16];
            fmt_tokens(tk, sizeof tk, p->lm_tok_today);
            snprintf(rq, sizeof rq, "%d", (int)p->lm_req_today);
            lv_label_set_text(cost_big, tk);
            lv_label_set_text(cost_tok, rq);
            lv_label_set_text(cost_tok_unit, "requests");
            lv_obj_align_to(cost_tok_unit, cost_tok, LV_ALIGN_OUT_RIGHT_BOTTOM, 5, 0);
            fmt_tokens(tk30, sizeof tk30, p->lm_tok_month_max);
            snprintf(rq30, sizeof rq30, "%d", (int)p->lm_req_month_max);
            lv_label_set_text_fmt(cost_30, "30 DAY MAX: %s Toks " LV_SYMBOL_BULLET "  %s Reqs", tk30, rq30);
            int n = p->lm_ht_n;
            if (n > NAV_HIST_PTS) n = NAV_HIST_PTS;
            lv_color_t cc;
            // Convert int64_t token history to int32_t for the chart renderer
            int32_t ht32[STATS_HIST_MAX];
            for (int hi = 0; hi < n && hi < STATS_HIST_MAX; hi++)
                ht32[hi] = (int32_t)(p->lm_ht[hi] > INT32_MAX ? INT32_MAX : p->lm_ht[hi]);
            (void)render_cost_bar_chart(cost_chart, cost_ser, ht32, n,
                prov_accent(p->id, &cc) ? cc : lv_color_hex(0x7C3AED));
            if (card_entered) anim_chart_fadein(cost_chart);
            return;
        }

        if (is_cursor && p->has_cu) {
            // CURSOR TODAY: token hero only (no requests line, no $, no OR rows)
            lv_obj_t *hide[] = { cost_or_lbl, cost_or_row1, cost_or_row2,
                                 cost_bar, cost_bar_lbl, cost_cap,
                                 cost_tok, cost_tok_unit };
            for (unsigned i = 0; i < sizeof hide / sizeof *hide; i++)
                lv_obj_add_flag(hide[i], LV_OBJ_FLAG_HIDDEN);
            cost_hero_caption(&hero, "TOKENS");
            lv_obj_t *show[] = { cost_big, cost_30, cost_chart };
            for (unsigned i = 0; i < sizeof show / sizeof *show; i++)
                lv_obj_clear_flag(show[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(cost_big, hero.x + 12, hero.y + 28);
            lv_obj_set_pos(cost_30, footer.x + 12, footer.y + 6);
            lv_obj_set_size(cost_chart, body.w - 24, body.h - 8);
            lv_obj_set_pos(cost_chart, body.x + 12, body.y + 4);
            char tk[16], tk30[16];
            fmt_tokens(tk, sizeof tk, p->cu_tok_today);
            lv_label_set_text(cost_big, tk);
            fmt_tokens(tk30, sizeof tk30, p->cu_tok_month_max);
            lv_label_set_text_fmt(cost_30, "30 DAY MAX: %s Toks", tk30);
            int n = p->cu_ht_n;
            if (n > NAV_HIST_PTS) n = NAV_HIST_PTS;
            lv_color_t cc;
            int32_t ht32[STATS_HIST_MAX];
            for (int hi = 0; hi < n && hi < STATS_HIST_MAX; hi++)
                ht32[hi] = (int32_t)(p->cu_ht[hi] > INT32_MAX ? INT32_MAX : p->cu_ht[hi]);
            (void)render_cost_bar_chart(cost_chart, cost_ser, ht32, n,
                prov_accent(p->id, &cc) ? cc : lv_color_hex(0x7C3AED));
            if (card_entered) anim_chart_fadein(cost_chart);
            return;
        }

        // Hide the TOKENS label for non-LM-studio providers.
        lv_obj_add_flag(cost_lbl, LV_OBJ_FLAG_HIDDEN);

        if (has_balance) {
            // OpenRouter: today cost is hero; balance is secondary (token-count style);
            // this-week cost is a caption at the bottom reusing cost_30 (H-38).
            lv_obj_t *or_unused[] = { cost_cap, cost_chart, cost_bar, cost_bar_lbl,
                                      cost_or_lbl, cost_or_row1, cost_or_row2 };
            for (unsigned i = 0; i < sizeof or_unused / sizeof *or_unused; i++)
                lv_obj_add_flag(or_unused[i], LV_OBJ_FLAG_HIDDEN);
            char tod[16], bal[16], wk[16];
            fmt_money(tod, sizeof tod, p->cost_today_c);
            if (card_entered) {
                anim_count_up(cost_big, p->cost_today_c, count_cents_cb);
            } else {
                lv_label_set_text(cost_big, tod);
            }
            lv_obj_clear_flag(cost_big, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(cost_big, hero.x + 12, hero.y + 28);
            cost_hero_caption(&hero, "SPEND");
            fmt_money(bal, sizeof bal, p->credits_remaining_c);
            lv_label_set_text(cost_tok, bal);
            lv_obj_clear_flag(cost_tok, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(cost_tok_unit, "balance");
            lv_obj_align_to(cost_tok_unit, cost_tok, LV_ALIGN_OUT_RIGHT_BOTTOM, 5, 0);
            lv_obj_clear_flag(cost_tok_unit, LV_OBJ_FLAG_HIDDEN);
            fmt_money(wk, sizeof wk, p->cost_week_c);
            lv_label_set_text_fmt(cost_30, "THIS WEEK  %s", wk);
            lv_obj_clear_flag(cost_30, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(cost_30, footer.x + 12, footer.y + 6);
        } else {
            // Claude/Codex/Pi layout: shared money/token/chart widgets. Pi uses
            // today's reduced usage for the hero numbers and 30-day max metrics
            // in the bottom summary row.
            lv_obj_t *or_only[] = { cost_or_lbl, cost_or_row1, cost_or_row2 };
            for (unsigned i = 0; i < sizeof or_only / sizeof *or_only; i++)
                lv_obj_add_flag(or_only[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_t *body_widgets[] = { cost_big, cost_tok, cost_tok_unit, cost_30, cost_chart };
            for (unsigned i = 0; i < sizeof body_widgets / sizeof *body_widgets; i++)
                lv_obj_clear_flag(body_widgets[i], LV_OBJ_FLAG_HIDDEN);
            // Claude, Codex and Pi all caption the hero amount "SPEND".
            cost_hero_caption(&hero, "SPEND");
            if (is_pi) {
                lv_obj_add_flag(cost_cap, LV_OBJ_FLAG_HIDDEN);
                // 30 DAY MAX summary in row 9 — the strip just below the 8-row
                // grid (y=300), clearing the rows 4..8 chart above it.
                const ui_rect_t footer_r = ui_grid_span(&g, 0, 8, 2, 1);
                lv_obj_set_pos(cost_30, footer_r.x + 12, footer_r.y + 2);
            } else {
                lv_obj_clear_flag(cost_cap, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(cost_cap, footer.x + 12, footer.y + 4);
                lv_obj_set_pos(cost_30, footer.x + 12, footer.y + 20);
            }
            if (is_pi) {
                // Pi chart spans grid rows 4..8 (1-indexed) — ui_grid_span row 3,
                // 5 rows tall.
                const ui_rect_t chart_r = ui_grid_span(&g, 0, 3, 2, 5);
                lv_obj_set_size(cost_chart, chart_r.w - 24, chart_r.h - 8);
                lv_obj_set_pos(cost_chart, chart_r.x + 12, chart_r.y + 4);
            } else {
                lv_obj_set_size(cost_chart, body.w - 24, body.h - 30);
                lv_obj_set_pos(cost_chart, body.x + 12, body.y + 4);
            }
            char m[16], tk[16], m30[16], tk30[16];
            fmt_money(m, sizeof m, p->cost_today_c);
            if (card_entered) {
                anim_count_up(cost_big, p->cost_today_c, count_cents_cb);
            } else {
                lv_label_set_text(cost_big, m);
            }
            fmt_tokens(tk, sizeof tk, p->tok_today);
            lv_label_set_text(cost_tok, tk);
            lv_label_set_text(cost_tok_unit, "tokens");
            lv_obj_align_to(cost_tok_unit, cost_tok, LV_ALIGN_OUT_RIGHT_BOTTOM, 5, 0);
            fmt_money(m30, sizeof m30, p->cost_month_c);
            fmt_tokens(tk30, sizeof tk30, p->tok_month);
            if (is_pi) {
                lv_label_set_text_fmt(cost_30, "30 DAY MAX: %s  " LV_SYMBOL_BULLET "  %s Toks",
                                      m30, tk30);
            } else {
                lv_label_set_text_fmt(cost_30, "30 DAYS TOTAL: %s  " LV_SYMBOL_BULLET "  %s Toks",
                                      m30, tk30);
            }
            int n = p->hist_n;
            if (n > NAV_HIST_PTS) n = NAV_HIST_PTS;
            lv_color_t cc;
            int32_t mx = render_cost_bar_chart(cost_chart, cost_ser, p->hist, n,
                prov_accent(p->id, &cc) ? cc : lv_color_hex(0xe06c4b));
            if (card_entered) anim_chart_fadein(cost_chart);
            if (!is_pi) {
                char cmx[16];
                fmt_money(cmx, sizeof cmx, mx);
                lv_label_set_text_fmt(cost_cap, "%d DAY SPEND (max): %s", n, cmx);
            }
        }
        return;
    }

    // CARD_LIMITS — works for any provider (uses p/pr/s/sr already on device)
    lv_obj_add_flag(cost_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lim_card, LV_OBJ_FLAG_HIDDEN);
    char pb[12];

    if (is_lmstudio) {
        // LM Studio STATS: tokens % (session) + requests % (weekly)
        {
            char up[STATS_ID_MAX];
            up_id(up, sizeof up, p->id);
            render_page_chrome(lim_hdr, lim_logo, &(ui_page_chrome_desc_t){
                .title = up,
                .subtitle = "STATS",
                .icon_id = p->id,
            });
        }
        lv_obj_set_pos(lim_s_lbl, hero.x + 12, hero.y + 14);
        lv_obj_set_pos(lim_s_big, hero.x + 12, hero.y + 28);
        lv_obj_set_pos(lim_s_bar, hero.x + 12, hero.y + 84);
        lv_obj_set_pos(lim_s_rst, hero.x + 12, hero.y + 96);
        lv_obj_set_size(lim_chart, body.w - 24, body.h - 10);
        lv_obj_set_pos(lim_chart, body.x + 12, body.y + 4);
        lv_obj_set_pos(lim_w_lbl, footer.x + 12, footer.y + 4);
        lv_obj_set_pos(lim_w_big, footer.x + 12, footer.y + 18);
        lv_obj_set_pos(lim_w_bar, footer.x + 12, footer.y + 44);
        lv_obj_set_pos(lim_w_rst, footer.x + 12, footer.y + 54);
        lv_obj_add_flag(lim_a_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lim_a_big, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lim_a_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lim_a_rst, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lim_chart, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lim_x_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lim_x_val, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lim_x_bar, LV_OBJ_FLAG_HIDDEN);
        fmt_pct(pb, sizeof pb, p->has_lm, p->p);
        lv_label_set_text(lim_s_lbl, "TOKENS");
        if (card_entered && p->has_lm) {
            anim_count_up(lim_s_big, (int32_t)(p->p * 10.0f + 0.5f), count_pct_cb);
        } else {
            lv_label_set_text(lim_s_big, pb);
        }
        set_bar(lim_s_bar, p->has_lm, p->p, p);
        lv_obj_add_flag(lim_s_rst, LV_OBJ_FLAG_HIDDEN);
        // Weekly = requests %
        fmt_pct(pb, sizeof pb, p->has_lm, p->s);
        if (p->has_lm) {
            lv_obj_clear_flag(lim_w_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lim_w_big, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lim_w_bar, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(lim_w_lbl, "REQUESTS");
            lv_label_set_text(lim_w_big, pb);
            set_bar(lim_w_bar, true, p->s, p);
        } else {
            lv_obj_add_flag(lim_w_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lim_w_big, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lim_w_bar, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_add_flag(lim_w_rst, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    {
        char up[STATS_ID_MAX];
        up_id(up, sizeof up, p->id);
        render_page_chrome(lim_hdr, lim_logo, &(ui_page_chrome_desc_t){
            .title = up,
            .subtitle = "LIMITS",
            .icon_id = p->id,
        });
    }
    lv_obj_set_pos(lim_s_lbl, hero.x + 12, hero.y + 14);
    lv_obj_set_pos(lim_s_big, hero.x + 12, hero.y + 28);
    lv_obj_set_pos(lim_s_bar, hero.x + 12, hero.y + 84);
    lv_obj_set_pos(lim_s_rst, hero.x + 12, hero.y + 96);
    lv_obj_set_size(lim_chart, body.w - 24, body.h - 10);
    lv_obj_set_pos(lim_chart, body.x + 12, body.y + 4);
    lv_obj_set_pos(lim_a_lbl, body.x + 12, body.y + 4);
    lv_obj_set_pos(lim_a_big, body.x + 12, body.y + 18);
    lv_obj_set_pos(lim_a_bar, body.x + 12, body.y + 44);
    lv_obj_set_pos(lim_a_rst, body.x + 12, body.y + 54);
    lv_obj_set_pos(lim_w_lbl, footer.x + 12, footer.y + 4);
    lv_obj_set_pos(lim_w_big, footer.x + 12, footer.y + 18);
    lv_obj_set_pos(lim_w_bar, footer.x + 12, footer.y + 44);
    lv_obj_set_pos(lim_w_rst, footer.x + 12, footer.y + 54);
    lv_obj_set_pos(lim_x_lbl, footer.x + 12, footer.y + 74);
    lv_obj_set_pos(lim_x_val, footer.x + 12, footer.y + 74);
    lv_obj_set_pos(lim_x_bar, footer.x + 12, footer.y + 90);

    lv_label_set_text(lim_s_lbl, has_balance ? "API KEY" : (p->has_t ? "TOTAL" : "SESSION"));
    fmt_pct(pb, sizeof pb, p->has_p, p->p);
    if (card_entered && p->has_p) {
        anim_count_up(lim_s_big, (int32_t)(p->p * 10.0f + 0.5f), count_pct_cb);
    } else {
        lv_label_set_text(lim_s_big, pb);
    }
    set_bar(lim_s_bar, p->has_p, p->p, p);
    if (has_balance) {
        lv_obj_add_flag(lim_s_rst, LV_OBJ_FLAG_HIDDEN);
    } else {
        set_reset_lbl(lim_s_rst, p->pr);
    }

    // Auto section: occupies chart area when no sparkline data and secondary exists.
    if (p->pct_hist_n == 0 && p->has_s) {
        lv_obj_clear_flag(lim_a_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lim_a_big, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lim_a_bar, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lim_a_lbl, p->has_t ? "AUTO" : "WEEKLY");
        fmt_pct(pb, sizeof pb, p->has_s, p->s);
        lv_label_set_text(lim_a_big, pb);
        set_bar(lim_a_bar, p->has_s, p->s, p);
        set_reset_lbl(lim_a_rst, p->sr);
    } else {
        lv_obj_add_flag(lim_a_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lim_a_big, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lim_a_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lim_a_rst, LV_OBJ_FLAG_HIDDEN);
    }

    // lim_w: "API" tier for providers with tertiary + no sparkline (Cursor).
    // Claude has pct_hist_n > 0; its secondary is the weekly window, not API.
    if (p->has_t && p->pct_hist_n == 0) {
        lv_obj_clear_flag(lim_w_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lim_w_big, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lim_w_bar, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lim_w_lbl, "API");
        fmt_pct(pb, sizeof pb, p->has_t, p->t);
        lv_label_set_text(lim_w_big, pb);
        set_bar(lim_w_bar, p->has_t, p->t, p);
        set_reset_lbl(lim_w_rst, p->tr);
    } else if (p->has_s && p->pct_hist_n > 0) {
        lv_obj_clear_flag(lim_w_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lim_w_big, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lim_w_bar, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lim_w_lbl, "WEEKLY");
        fmt_pct(pb, sizeof pb, p->has_s, p->s);
        lv_label_set_text(lim_w_big, pb);
        set_bar(lim_w_bar, p->has_s, p->s, p);
        set_reset_lbl(lim_w_rst, p->sr);
    } else {
        lv_obj_add_flag(lim_w_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lim_w_big, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lim_w_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lim_w_rst, LV_OBJ_FLAG_HIDDEN);
    }

    if (p->has_cost && p->extra_limit_c > 0) {
        lv_obj_clear_flag(lim_x_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lim_x_val, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lim_x_bar, LV_OBJ_FLAG_HIDDEN);
        int xp = extra_pct(p);
        if (has_balance) {
            // Budget bar: shows remaining headroom (xu/xl = keyUsage/keyLimit).
            // Bar fills proportional to remaining so more-left = more-filled.
            int32_t rem = p->extra_limit_c - p->extra_used_c;
            if (rem < 0) rem = 0;
            char rem_str[16], lim_str[16];
            fmt_money(rem_str, sizeof rem_str, rem);
            fmt_money(lim_str, sizeof lim_str, p->extra_limit_c);
            lv_label_set_text(lim_x_lbl, "BUDGET");
            lv_label_set_text_fmt(lim_x_val, "%s / %s left", rem_str, lim_str);
            lv_bar_set_value(lim_x_bar, 100 - xp, LV_ANIM_ON);
            lv_obj_set_style_bg_color(lim_x_bar, bar_color(p, (float)xp),
                                      LV_PART_INDICATOR);
        } else {
            char a[16], b[16];
            fmt_money(a, sizeof a, p->extra_used_c);
            fmt_money(b, sizeof b, p->extra_limit_c);
            lv_label_set_text(lim_x_lbl, "EXTRA USAGE");
            lv_label_set_text_fmt(lim_x_val, "%s / %s", a, b);
            lv_bar_set_value(lim_x_bar, bar_fill(xp), LV_ANIM_ON);
            lv_obj_set_style_bg_color(lim_x_bar, bar_color(p, (float)xp),
                                      LV_PART_INDICATOR);
        }
        update_bar_pulse(lim_x_bar, (float)xp);
    } else {
        update_bar_pulse(lim_x_bar, 0.0f);
        lv_obj_add_flag(lim_x_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lim_x_val, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lim_x_bar, LV_OBJ_FLAG_HIDDEN);
    }

    // 24h SESSION usage-% sparkline from `ph` (Claude only; absent elsewhere).
    if (p->pct_hist_n > 0) {
        lv_obj_clear_flag(lim_chart, LV_OBJ_FLAG_HIDDEN);
        int n = p->pct_hist_n;
        if (n > STATS_PCT_HIST_MAX) n = STATS_PCT_HIST_MAX;
        lv_chart_set_point_count(lim_chart, (uint32_t)n);
        lv_chart_set_range(lim_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
        lv_color_t lc;
        lv_chart_set_series_color(lim_chart, lim_ser,
            prov_accent(p->id, &lc) ? lc : lv_color_hex(0x30c14e));
        for (int i = 0; i < n; i++)
            lv_chart_set_value_by_id(lim_chart, lim_ser, i, p->pct_hist[i]);
        lv_chart_refresh(lim_chart);
        if (card_entered) anim_chart_fadein(lim_chart);
    } else {
        lv_obj_add_flag(lim_chart, LV_OBJ_FLAG_HIDDEN);
    }
}

static void render(void)   // ui_task only
{
    if (st.mode == UI_PROVISION) {
        hide_cards();
        lv_obj_clear_flag(title,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(status, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < ROWS; i++) {
            lv_obj_add_flag(row_id[i],   LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(row_bar[i],  LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(row_val[i],  LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(row_icon[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_clear_flag(prov_box, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(title, "SETUP");
        lv_label_set_text(status, "Join this WiFi, then open 192.168.4.1");
        lv_label_set_text_fmt(prov_box,
            st.prov_wifi_only
              ? "WiFi:  %s\nPass:  %s\n\nAdd a WiFi network.\nUpstash is already set."
              : "WiFi:  %s\nPass:  %s\n\nThen enter your home WiFi +\nUpstash URL + read-only token.",
            st.ssid, st.pass);
        led_off();
        return;
    }

    // ---- UI_STATS: dispatch on the navigation level --------------------
    // A stats refresh may have dropped OR REORDERED the provider we drilled
    // into. Re-resolve the page by stored id (not by stale index) on ui_task
    // under s_mtx, before any read of nav_provider: a reorder that keeps the
    // same count must not silently swap which provider the page shows; a
    // drop-out falls back to the summary. (Audit QA§P1-2.) A transient
    // ok:false is NOT ejected — the Limits page still renders and the Cost
    // page shows its placeholder, so don't bounce the user out.
    if (st.nav_level == NAV_PAGE) {
        int j = -1;
        for (int i = 0; i < st.stats.n && i < STATS_MAX_PROVIDERS; i++)
            if (strcmp(st.stats.p[i].id, st.nav_id) == 0) { j = i; break; }
        if (j < 0) st.nav_level = NAV_SUMMARY;   // provider gone
        else       st.nav_provider = j;          // follow reorder by identity
    }
    clamp_scroll();

    if (st.nav_level == NAV_PAGE)
        led_set_provider(st.nav_id);
    else
        led_off();

    if (st.nav_level == NAV_PAGE) {
        render_card();
        return;
    }

    // NAV_SUMMARY — scrollable provider list
    hide_cards();
    s_prev_nav_level = NAV_SUMMARY;   // ensure next card entry re-triggers animations
    lv_obj_add_flag(title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(prov_box, LV_OBJ_FLAG_HIDDEN);

    // Scrollable window: `vis` rows fit; st.scroll is the top visible-provider
    // index in the compact list (hidden providers do not consume slots).
    // ASCII-only " +N more" hint (font ships 0x20-0x7F + 0xB0 + 0x2022 only)
    // when the list is longer than the screen, so it never looks truncated.
    int vis  = summary_vis_rows();
    int shown_n = summary_visible_count();
    int more = shown_n - (st.scroll + vis);       // rows hidden BELOW current window
    if (more < 0) more = 0;
    char hint[24];   // "  +" + up to 11-digit %d + " more" + NUL (-Werror
                      // format-truncation is static; size for the worst %d)
    hint[0] = '\0';
    if (more > 0) snprintf(hint, sizeof hint, "  +%d more", more);

    // Audit State§HIGH/MED: compose the age suffix HERE from st.fetched_ms
    // (a local buffer — never mutate shared st.status). This removes the old
    // save/restore-under-mutex hack and the freshness gap where the counter
    // froze ~10 s after a fetch.
    if (st.fetched_ms > 0) {
        int age = (int)((esp_timer_get_time() / 1000 - st.fetched_ms) / 1000);
        if (age < 0) age = 0;
        char line[128];  // st.status(<=63) + " • updated <int>s ago" + hint
        // Audit UI§HIGH: separator must be a glyph compiled into
        // lv_font_montserrat_12. That font ships with only 0x20-0x7F,
        // 0xB0, 0x2022 (see lv_font_montserrat_12.c gen opts), so the old
        // U+00B7 MIDDLE DOT (·) had no glyph and rendered as a tofu box.
        // LV_SYMBOL_BULLET is U+2022, which *is* in the font.
        snprintf(line, sizeof line,
                 "%s " LV_SYMBOL_BULLET " updated %ds ago%s",
                 st.status, age, hint);
        lv_label_set_text(status, line);
    } else if (more > 0) {
        char line[96];   // st.status(<=63) + hint(<=23) + NUL
        snprintf(line, sizeof line, "%s%s", st.status, hint);
        lv_label_set_text(status, line);
    } else {
        lv_label_set_text(status, st.status);
    }

    const ui_page_grid_t g = ui_grid_from_height(s_scr_w, s_scr_h);
    for (int i = 0; i < ROWS; i++) {
        int pi = summary_provider_at(st.scroll + i); // i = visual slot, pi = stats.p[] index
        if (i >= vis || pi < 0) {
            lv_obj_add_flag(row_id[i],   LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(row_bar[i],  LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(row_val[i],  LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(row_icon[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(row_bar_w[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const stats_provider_t *p = &st.stats.p[pi];
        lv_obj_clear_flag(row_id[i],  LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(row_val[i], LV_OBJ_FLAG_HIDDEN);
        {
            const ui_rect_t r = ui_grid_span(&g, 0, i, 2, 1);
            bool cu_warn = cursor_sess_refresh_needed(p);
            lv_obj_set_size(row_icon[i], 32, 32);
            lv_obj_set_pos(row_icon[i], r.x + 8, r.y + 1);
            lv_obj_set_pos(row_id[i], r.x + ROW_TXT_X, r.y + 2);
            lv_obj_set_width(row_id[i], r.w - ROW_TXT_X - 8);
            lv_obj_set_pos(row_bar[i], r.x + ROW_TXT_X, r.y + 21);
            lv_obj_set_size(row_bar[i], r.w - ROW_TXT_X - 60, 5);
            lv_obj_set_pos(row_val[i], r.x + r.w - 52, r.y + 15);
            lv_obj_set_pos(row_bar_w[i], r.x + ROW_TXT_X, r.y + 28);
            lv_obj_set_size(row_bar_w[i], r.w - ROW_TXT_X - 60, 2);
            lv_label_set_text(row_id[i], p->id);
            lv_obj_set_style_text_color(row_id[i],
                cu_warn ? lv_color_hex(CURSOR_SESS_AMBER) : lv_color_hex(0xe8eaed), 0);
        }

        // Provider logo: A8 silhouette (tinted via recolor) or ARGB8888
        // full-color image (no tinting). Hidden if no icon for this id.
        const lv_image_dsc_t *ic = provider_summary_icon(p->id);
        if (ic) {
            lv_image_set_src(row_icon[i], ic);
            if (provider_icon_is_full_color(p->id)) {
                lv_obj_set_style_image_recolor_opa(
                    row_icon[i], LV_OPA_TRANSP, 0);
            } else {
                lv_color_t tc;
                lv_obj_set_style_image_recolor_opa(
                    row_icon[i], LV_OPA_COVER, 0);
                lv_obj_set_style_image_recolor(row_icon[i],
                    prov_accent(p->id, &tc) ? tc : lv_color_hex(0xe8eaed), 0);
            }
            lv_obj_clear_flag(row_icon[i], LV_OBJ_FLAG_HIDDEN);
            update_cursor_sess_pulse(row_icon[i], cursor_sess_refresh_needed(p));
        } else {
            lv_obj_add_flag(row_icon[i], LV_OBJ_FLAG_HIDDEN);
            update_cursor_sess_pulse(row_icon[i], false);
        }

        if (!p->ok || !p->has_p) {
            update_bar_pulse(row_bar[i], 0.0f);
            lv_obj_add_flag(row_bar[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_color(row_val[i], lv_color_hex(0x6b7075), 0);
            lv_label_set_text(row_val[i], "off");
        } else {
            int v = (int)(p->p + 0.5f);
            if (v < 0) v = 0; else if (v > 100) v = 100;
            int fill = bar_fill(v);
            lv_obj_clear_flag(row_bar[i], LV_OBJ_FLAG_HIDDEN);
            if (fill != s_prev_row_bar[i]) {
                lv_bar_set_value(row_bar[i], fill, LV_ANIM_ON);
                s_prev_row_bar[i] = fill;
            }
            // Per-provider accent (Claude orange); un-themed providers keep
            // the green/amber/red usage ramp.
            lv_obj_set_style_bg_color(row_bar[i], bar_color(p, p->p), LV_PART_INDICATOR);
            update_bar_pulse(row_bar[i], p->p);
            lv_obj_set_style_text_color(row_val[i], lv_color_hex(0xffffff), 0);
            {
                int tenths = (int)(p->p * 10.0f + 0.5f);
                if (tenths < 0) tenths = 0; else if (tenths > 1000) tenths = 1000;
                lv_label_set_text_fmt(row_val[i], "%d.%d%%",
                                      tenths / 10, tenths % 10);
            }
            // Weekly bar under session bar (Claude / Codex / LM Studio).
            // For LM Studio, `s` is requests % (vs 30-day max).
            if (((strcmp(p->id, "claude") == 0 || strcmp(p->id, "codex") == 0)
                 && p->has_s)
                || strcmp(p->id, "lmstudio") == 0) {
                int wv = (int)(p->s + 0.5f);
                if (wv < 0) wv = 0; else if (wv > 100) wv = 100;
                lv_bar_set_value(row_bar_w[i], bar_fill(wv), LV_ANIM_ON);
                lv_obj_set_style_bg_color(row_bar_w[i], bar_color(p, p->s), LV_PART_INDICATOR);
                update_bar_pulse(row_bar_w[i], p->s);
                lv_obj_clear_flag(row_bar_w[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(row_bar_w[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

static void ui_task(void *arg)
{
    (void)arg;
    build_widgets();
    int64_t next_age = 0;
    while (1) {
        bool do_shot = false;
        // Audit State§HIGH: block briefly for the mutex rather than skipping
        // the render entirely (take(0)) under setter contention.
        if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(5)) == pdTRUE) {
            int64_t now = esp_timer_get_time() / 1000;
            // Screensaver timers.
            if (st.mode != UI_STATS) { st.saver_active = false; st.saver_fade_end_ms = 0; st.saver_transitioning = false; st.saver_show_summary = false; st.saver_next_show_summary = false; }
            saver_step_fade_locked(now);
            if (st.mode == UI_STATS && !st.saver_active && st.last_input_ms > 0 && now - st.last_input_ms >= SCREENSAVER_IDLE_MS)
                saver_enter_locked(now);
            saver_advance_locked(now);
            // Re-render every 10 s in stats mode so the "updated Ns ago"
            // counter ticks even without new data. render() recomputes the
            // age from st.fetched_ms, so this is always accurate (no gap).
            if (st.mode == UI_STATS && st.fetched_ms > 0 && now >= next_age) {
                next_age = now + 10000;
                st.dirty = true;
            }
            if (st.dirty) { render(); st.dirty = false; }
            do_shot = st.shot_req;
            if (do_shot) st.shot_req = false;
            xSemaphoreGive(s_mtx);
        }
        lv_timer_handler();
        if (do_shot && scr) {
            // Force a full synchronous re-render so the shadow framebuffer in
            // display.c has a complete, up-to-date copy of every pixel.
            lv_obj_invalidate(scr);
            lv_refr_now(lv_display_get_default());
            xSemaphoreGive(s_shot_sem);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void ui_start(void)
{
    s_mtx     = xSemaphoreCreateMutex();
    s_shot_sem = xSemaphoreCreateBinary();
    st.mode = UI_STATS;
    st.nav_level = NAV_SUMMARY;
    strlcpy(st.status, "starting...", sizeof st.status);
    st.dirty = true;
    xTaskCreate(ui_task, "ui", 8192, NULL, 5, NULL);
}

bool ui_capture_screenshot(void)
{
    if (!s_shot_sem) return false;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    st.shot_req = true;
    xSemaphoreGive(s_mtx);
    // Block until the ui_task completes its forced full re-render (≤ ~50 ms).
    return xSemaphoreTake(s_shot_sem, pdMS_TO_TICKS(2000)) == pdTRUE;
}

static void mark(void) { st.dirty = true; }

void ui_set_provisioning(const char *ssid, const char *pass, bool wifi_only)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    st.mode = UI_PROVISION;
    st.nav_level = NAV_SUMMARY;          // leave any open menu/card
    st.prov_wifi_only = wifi_only;
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

void ui_set_bar_invert(bool on)
{
    // s_bar_invert is read by render() on ui_task; guard + mark dirty like
    // the other setters so a runtime change (portal/NVS) repaints at once.
    // Tolerate a call before ui_start() (s_mtx not yet created).
    if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_bar_invert = on;
    if (s_mtx) { mark(); xSemaphoreGive(s_mtx); }
}

void ui_set_stats(const stats_t *s, int64_t fetched_uptime_ms)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    st.mode = UI_STATS;
    if (s) { update_provider_activity_locked(s, fetched_uptime_ms); st.stats = *s; stats_model_reorder(&st.stats); }
    st.fetched_ms = fetched_uptime_ms;
    mark();
    xSemaphoreGive(s_mtx);
}

// Navigation state machine (ARCHITECTURE.md decision #9: nav lives in ui.c).
// Runs on the CALLER's task (fetch_task), mutating only `st` under s_mtx —
// exactly like the setters above; no LVGL call here (summary_hit_test /
// clamp_scroll are pure reads/writes of `st` + the cached height).
// Returns UI_INPUT_PASS ONLY for a LONG_PRESS on the summary (→ fetch.c
// opens the add-network portal) or any event in provisioning mode. Every
// other event is consumed by the nav machine.
ui_input_result_t ui_handle_input(const app_evt_t *ev)
{
    if (!ev) return UI_INPUT_PASS;
    ui_input_result_t r = UI_INPUT_CONSUMED;
    xSemaphoreTake(s_mtx, portMAX_DELAY);

    if (st.mode != UI_STATS) {           // provisioning: passthrough
        r = UI_INPUT_PASS;
        goto out;
    }

    st.last_input_ms = esp_timer_get_time() / 1000;
    if (st.saver_active) { saver_exit_locked(st.last_input_ms); r = UI_INPUT_CONSUMED; goto out; }

    switch (st.nav_level) {
    case NAV_SUMMARY:
        if (ev->type == APP_EVT_SWIPE_UP) {           // page down the list
            st.scroll += summary_vis_rows();
            clamp_scroll();
            st.dirty = true;
        } else if (ev->type == APP_EVT_SWIPE_DOWN) {  // page up the list
            st.scroll -= summary_vis_rows();
            clamp_scroll();
            st.dirty = true;
        } else if (ev->type == APP_EVT_TAP) {
            int pi = summary_hit_test(ev->y);
            if (pi >= 0) {
                const stats_provider_t *tp = &st.stats.p[pi];
                st.nav_provider = pi;
                strlcpy(st.nav_id, tp->id, sizeof st.nav_id);
                // LM Studio / Cursor (cu) open CARD_COST (TODAY); others open Cost if available
                if (strcmp(tp->id, "lmstudio") == 0
                    || (strcmp(tp->id, "cursor") == 0 && tp->has_cu)) {
                    st.nav_card = CARD_COST;
                } else {
                    st.nav_card = tp->has_cost ? CARD_COST : CARD_LIMITS;
                }
                st.nav_level = NAV_PAGE;
                st.dirty = true;
            }
        } else if (ev->type == APP_EVT_LONG_PRESS) {
            r = UI_INPUT_PASS;                         // → enter_portal()
        }
        /* SWIPE_LEFT at the root: swallow (CONSUMED, no-op) */
        break;

    case NAV_PAGE:
        if (ev->type == APP_EVT_TAP) {
            // All providers: 2-card toggle (like other providers).
            const stats_provider_t *np = &st.stats.p[st.nav_provider];
            if (strcmp(np->id, "lmstudio") == 0
                || (strcmp(np->id, "cursor") == 0 && np->has_cu)) {
                // Both pages always present — unconditional toggle so taps
                // keep cycling Today <-> Stats / Today <-> Limits.
                st.nav_card = (st.nav_card == CARD_COST) ? CARD_LIMITS : CARD_COST;
            } else {
                st.nav_card = (st.nav_card == CARD_COST) ? CARD_LIMITS
                    : (np->has_cost ? CARD_COST : CARD_LIMITS);
            }
            st.dirty = true;
        } else if (ev->type == APP_EVT_SWIPE_LEFT) {   // back to the list
            st.nav_level = NAV_SUMMARY;
            st.dirty = true;
        }
        /* swipe up/down/long-press on a page: swallowed (CONSUMED) */
        break;
    }

out:
    xSemaphoreGive(s_mtx);
    return r;
}
