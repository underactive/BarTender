// firmware/main/ui_internal.h
//
// PRIVATE shared header for the ui_*.c translation units (ui.c, ui_format.c,
// ui_render.c, ui_screensaver.c). NOT part of the public API — callers use
// ui.h. This header carries the types, tunables, shared mutable state (declared
// `extern`; defined in exactly one .c), and the formerly-file-static functions
// that became cross-file after ui.c was split.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"
#include "stats_model.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

extern const lv_font_t font_lemonmilk_48;
extern const lv_font_t font_lemonmilk_36;
extern const lv_font_t font_lemonmilk_24;
extern const lv_font_t font_lemonmilk_23;

// ── Tunables / layout constants ──────────────────────────────────────────────
#define ROWS         STATS_MAX_PROVIDERS
#define ROW_Y0       20
#define ROW_H        48          // icon column + name line + bar/% line
#define ROW_ICON_PX  32          // matches scripts/build/gen-provider-icons.py
#define ROW_TXT_X    48          // name/bar start (right of the icon column)
// Per-provider page watermark: 32px icon at 10x, top-right, gaussian-blurred.
#define PAGE_BG_ICON_SCALE   (256 * 10)   // LVGL image scale: 256 == 1.0x
#define PAGE_BG_BLUR_RADIUS  67        // chosen for visual balance at 10x scale
#define PAGE_BG_ICON_OPA     ((lv_opa_t)((255 * 23) / 100))  /* ~23%; no LV_OPA_23 */

#define NAV_HIST_PTS STATS_HIST_MAX   // chart points == payload schema cap,
                                      // NOT a UI choice (keep them equal)

#define SCREENSAVER_IDLE_MS     (5LL * 60LL * 1000LL)
#define SCREENSAVER_ACTIVE_MS   (8LL * 60LL * 60LL * 1000LL)
#define SCREENSAVER_PAGE_MS     (15LL * 1000LL)
#define SCREENSAVER_FADE_MS     700LL
#define SCREENSAVER_DIM_DUTY    8
#define BOOT_FADE_MS            600LL

// Named cadence / animation durations
#define AGE_TICK_MS              10000LL   // re-render interval for "updated Ns ago"
#define LED_TICK_INTERVAL_MS     40LL      // LED update granularity (limits blocking RMT writes)
#define BAR_PULSE_ANIM_MS        700       // bar heartbeat half-period (≥90 % usage)
#define CURSOR_PULSE_ANIM_MS     1200      // cursor-icon amber pulse half-period
#define HERO_COUNTUP_MS          400       // count-up animation duration
#define CHART_FADEIN_MS          600       // chart fade-in duration on card entry

// Fallback brand color for providers without a registered accent (lmstudio).
#define UI_DEFAULT_CHART_COLOR   0x7C3AED

#define CURSOR_SESS_AMBER 0xF4A261

// Providers hidden from the summary page. Add/remove IDs here to toggle
// visibility — no structural changes needed.
#define HIDDEN_PROVIDERS "ollama", "opencode"

#define UI_GRID_COLS     2
#define UI_GRID_ROWS     8
#define UI_CHROME_TOP    20
#define UI_CHROME_BOTTOM 16
#define UI_SUMMARY_GAP      8
#define UI_SUMMARY_TOP_ROWS 2   // content-grid rows reserved above provider list
#define UI_GRID_COLOR    0x8da4c0
#define UI_GRID_OPA      LV_OPA_90

// Bar fill direction. true => 0% draws FULL, 100% draws EMPTY (bars read as
// "headroom remaining"). Flip this compile-time default to change it globally.
#define UI_BAR_INVERT_DEFAULT  false

// ── Types ────────────────────────────────────────────────────────────────────
typedef enum { UI_PROVISION, UI_STATS } ui_mode_t;
// Sub-state of UI_STATS: the scrollable summary list, or a per-provider page
// (Cost/Limit, toggled by tap; entered by tapping a summary row).
typedef enum { NAV_SUMMARY, NAV_PAGE } nav_level_t;
typedef enum { CARD_COST, CARD_LIMITS } card_kind_t;

typedef enum { BOOT_FADE_NONE, BOOT_FADE_OUT, BOOT_FADE_IN } boot_fade_t;

// Provider identity as an enum so call sites switch/compare instead of repeating
// strcmp ladders. Add new entries here; unknown ids map to PK_UNKNOWN and fall
// through the default branch.
typedef enum {
    PK_UNKNOWN = 0,
    PK_PI,
    PK_CLAUDE,
    PK_CODEX,
    PK_LMSTUDIO,
    PK_CURSOR,
    PK_OPENCODEGO,
    PK_OPENROUTER,
} provider_kind_t;

typedef struct {
    char               id[STATS_ID_MAX];
    bool               seen;      // seen this cycle
    bool               has_sig;   // has a valid sig (not all-zero)
    uint32_t           sig;       // hash of provider id for comparison
    int64_t            last_change_ms;
} saver_activity_t;

typedef struct { int x, y, w, h; } ui_rect_t;
typedef struct { ui_rect_t content; int cell_w; int cell_h; } ui_page_grid_t;

// A hero_amount is a caption + big number placed together as one unit, so the
// two can never drift apart. One instance per card.
typedef struct { lv_obj_t *caption; lv_obj_t *num; } hero_amount_t;

// Cost card widget group
typedef struct {
    lv_obj_t *card, *bg_logo, *hdr, *logo, *tok, *tok_unit;
    lv_obj_t *cost_30, *bar, *bar_lbl, *na, *cap;
    lv_obj_t *chart;
    lv_chart_series_t *ser;
    // OpenRouter balance layout (balance hero + today/week secondary rows)
    lv_obj_t *or_lbl, *or_row1, *or_row2;
} cost_card_t;

// Usage-Limits card widget group
typedef struct {
    lv_obj_t *card, *bg_logo, *hdr, *logo;
    lv_obj_t *s_bar, *s_rst;
    lv_obj_t *a_lbl, *a_big, *a_bar, *a_rst;
    lv_obj_t *w_lbl, *w_big, *w_bar, *w_rst;
    lv_obj_t *x_lbl, *x_val, *x_bar, *cap;
    lv_obj_t *chart;
    lv_chart_series_t *ser;
} lim_card_t;

typedef struct {
    const char *title;
    const char *subtitle;
    const char *icon_id;
} ui_page_chrome_desc_t;

// ── Shared mutable state ──────────────────────────────────────────────────────
// Owned by ui.c (definitions there); referenced by ui_render.c / ui_screensaver.c.
extern SemaphoreHandle_t s_mtx;
extern SemaphoreHandle_t s_shot_sem;

// Shared state — written by any task under s_mtx, consumed only by ui_task.
extern struct ui_state {
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
    bool locked;
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
    // Boot splash: full-grid image until first stats, then backlight fade.
    bool boot_complete;
    boot_fade_t boot_fade;
    uint8_t boot_brightness, boot_target_brightness;
    int64_t boot_fade_start_ms, boot_fade_end_ms;
} st;

// Cached screen size — defined in ui_render.c (set in build_widgets), read by
// ui.c nav helpers off ui_task (no LVGL call). NEVER mutated outside build_widgets.
extern int s_scr_w;
extern int s_scr_h;

// Card widget groups — defined in ui_render.c, referenced across render helpers.
extern cost_card_t cost;
extern lim_card_t  lim;
extern hero_amount_t cost_hero, lim_hero;

// Clamp an int to [lo, hi]. Shared by the formatting + render paths to replace
// the open-coded `if (v < lo) v = lo; else if (v > hi) v = hi;` idiom that was
// duplicated across ui_format.c / ui_render.c. static inline: each TU gets its
// own copy, no link conflict, identical codegen to the hand-written version.
static inline int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// ── Cross-file functions ─────────────────────────────────────────────────────
// ui_format.c — pure helpers (no st, no LVGL widget globals).
ui_page_grid_t ui_grid_from_height(int screen_w, int screen_h);
ui_rect_t ui_grid_span(const ui_page_grid_t *g, int col, int row, int cols, int rows);
int summary_vis_rows_from_grid(const ui_page_grid_t *g);
bool provider_has_limits_card(const stats_provider_t *p);
int pct_tenths(bool has, float v);
uint32_t hash_mix_u32(uint32_t h, uint32_t v);
uint32_t provider_metric_sig(const stats_provider_t *p);
bool provider_card_available(const stats_provider_t *p, card_kind_t card);
lv_color_t pct_color(float p);
bool prov_accent(const char *id, lv_color_t *out);
bool is_hidden_provider(const char *id);
provider_kind_t provider_kind(const char *id);
lv_color_t bar_color(const stats_provider_t *p, float v);
int bar_fill(int pct);
int64_t provider_tok_today(const stats_provider_t *p);
const char *summary_provider_name(const char *id);
void fmt_tokens(char *buf, size_t n, int64_t t);
void fmt_tokens_full(char *buf, size_t n, int64_t t);
void fmt_money(char *buf, size_t n, int32_t cents);
void fmt_pct(char *buf, size_t n, bool has, float v);
void up_id(char *dst, size_t n, const char *src);
int extra_pct(const stats_provider_t *p);
void i64_hist_to_i32(int32_t *dst, const int64_t *src, int n);

// ui.c — nav/summary geometry helpers (read st + cached size; mutate st.scroll).
int summary_vis_rows(void);
int summary_visible_count(void);
int summary_provider_at(int visible_idx);
void clamp_scroll(void);
int summary_hit_test(int y);
int64_t summary_tok_today_total(void);

// ui_screensaver.c — screensaver / boot backlight / idle dimming (all under s_mtx).
void update_provider_activity_locked(const stats_t *s, int64_t now_ms);
int find_provider_id(const char *id);
void boot_start_fade_locked(uint8_t from, uint8_t to, int64_t now);
void boot_step_fade_locked(int64_t now);
bool boot_splash_visible_locked(void);
void saver_step_fade_locked(int64_t now);
void saver_enter_locked(int64_t now);
void saver_advance_locked(int64_t now);
void saver_exit_locked(int64_t now);

// ui_render.c — LVGL widget construction + draw/update (ui_task only).
void build_widgets(void);
void render(void);
lv_obj_t *ui_active_screen(void);  // active screen, NULL before build_widgets()
