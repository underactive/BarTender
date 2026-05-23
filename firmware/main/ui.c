// firmware/main/ui.c
#include "ui.h"
#include "provider_icons.h"
#include "provider_colors.h"
#include "led.h"
#include "lvgl.h"
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
#define ROW_Y0       46
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
} st;

// Widgets (created once, mutated only on ui_task)
static lv_obj_t *scr, *title, *status, *prov_box;
static lv_obj_t *row_id[ROWS], *row_bar[ROWS], *row_val[ROWS], *row_icon[ROWS], *row_bar_w[ROWS];

// Cost card
static lv_obj_t *cost_card, *cost_hdr, *cost_logo, *cost_big, *cost_tok, *cost_tok_unit,
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

static int s_scr_h = 320;   // cached screen height (set in build_widgets);
                            // read by ui_handle_input -> NO LVGL call off-task

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
    int r = (s_scr_h - ROW_Y0) / ROW_H;     // 320px: (320-46)/48 = 5
    if (r < 1)    r = 1;
    if (r > ROWS) r = ROWS;
    return r;
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
    if (y < ROW_Y0) return -1;
    int slot = (y - ROW_Y0) / ROW_H;
    if (slot < 0 || slot >= summary_vis_rows()) return -1;
    if ((y - ROW_Y0) % ROW_H > ROW_H - 8) return -1;   // 8 px inter-row gap
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


static void create_card_hdr(lv_obj_t *card, lv_obj_t **hdr_out, lv_obj_t **logo_out);
static void render_card_hdr(lv_obj_t *hdr, lv_obj_t *logo, const char *id, const char *page);
static void bar_opa_cb(void *obj, int32_t opa);
static void update_bar_pulse(lv_obj_t *bar, float pct);

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
    s_scr_h = H;
    memset(s_prev_row_bar, -1, sizeof s_prev_row_bar);
    const int val_x = W - 52;          // right-anchored % column (line 2)

    title = lv_label_create(scr);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_label_set_text(title, "BARTENDER");
    lv_obj_set_pos(title, 8, 6);

    status = lv_label_create(scr);
    lv_obj_set_style_text_color(status, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(status, &lv_font_montserrat_12, 0);
    lv_label_set_text(status, "starting...");
    lv_obj_set_pos(status, 8, 28);

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

    cost_big = lv_label_create(cost_card);
    lv_obj_set_style_text_color(cost_big, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(cost_big, &font_lemonmilk_48, 0);
    lv_obj_set_pos(cost_big, 12, 32);

    cost_tok = lv_label_create(cost_card);
    lv_obj_set_style_text_color(cost_tok, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(cost_tok, &font_lemonmilk_24, 0);
    lv_obj_set_pos(cost_tok, 12, 88);

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
    lv_obj_set_size(cost_chart, W - 24, H - 166);
    lv_obj_set_pos(cost_chart, 12, 120);
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
    lv_obj_set_pos(*logo_out, 12, 11);
    lv_obj_set_style_image_recolor_opa(*logo_out, LV_OPA_COVER, 0);
    lv_image_set_pivot(*logo_out, 0, 0);
    lv_image_set_scale(*logo_out, 112);   // 32px * (112/256) ≈ 14px = font_14 height
    lv_obj_add_flag(*logo_out, LV_OBJ_FLAG_HIDDEN);

    *hdr_out = lv_label_create(card);
    lv_obj_set_style_text_color(*hdr_out, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(*hdr_out, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(*hdr_out, 12, 10);
}

static void render_card_hdr(lv_obj_t *hdr, lv_obj_t *logo,
                             const char *id, const char *page)
{
    char up[STATS_ID_MAX];
    up_id(up, sizeof up, id);
    lv_label_set_text_fmt(hdr, "%s  %s", up, page);

    lv_color_t tc;
    const lv_image_dsc_t *ic = provider_icon(id);
    if (ic) {
        lv_image_set_src(logo, ic);
        lv_obj_set_style_image_recolor(logo,
            prov_accent(id, &tc) ? tc : lv_color_hex(0xe8eaed), 0);
        lv_obj_clear_flag(logo, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(hdr, 30, 10);
    } else {
        lv_obj_add_flag(logo, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(hdr, 12, 10);
    }
}

static void hide_cards(void)     // hide the Cost/Limit panels (chrome stays)
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

    if (st.nav_card == CARD_COST) {
        lv_obj_add_flag(lim_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(cost_card, LV_OBJ_FLAG_HIDDEN);
        render_card_hdr(cost_hdr, cost_logo, p->id, is_pi ? "STATS" : "TODAY");

        if (!p->has_cost) {
            lv_obj_clear_flag(cost_na, LV_OBJ_FLAG_HIDDEN);
            lv_obj_t *all[] = { cost_big, cost_tok, cost_tok_unit, cost_30, cost_cap,
                                cost_chart, cost_bar, cost_bar_lbl,
                                cost_or_lbl, cost_or_row1, cost_or_row2 };
            for (unsigned i = 0; i < sizeof all / sizeof *all; i++)
                lv_obj_add_flag(all[i], LV_OBJ_FLAG_HIDDEN);
            return;
        }
        lv_obj_add_flag(cost_na, LV_OBJ_FLAG_HIDDEN);

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
            fmt_money(bal, sizeof bal, p->credits_remaining_c);
            lv_label_set_text(cost_tok, bal);
            lv_obj_clear_flag(cost_tok, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(cost_tok_unit, "balance");
            lv_obj_align_to(cost_tok_unit, cost_tok, LV_ALIGN_OUT_RIGHT_BOTTOM, 5, 0);
            lv_obj_clear_flag(cost_tok_unit, LV_OBJ_FLAG_HIDDEN);
            fmt_money(wk, sizeof wk, p->cost_week_c);
            lv_label_set_text_fmt(cost_30, "THIS WEEK  %s", wk);
            lv_obj_clear_flag(cost_30, LV_OBJ_FLAG_HIDDEN);
        } else {
            // Claude/Codex/Pi layout: shared money/token/chart widgets, with
            // Pi relabeled because its `pi` block carries 30-day max metrics
            // rather than today's/30-day-total cost semantics.
            lv_obj_t *or_only[] = { cost_or_lbl, cost_or_row1, cost_or_row2 };
            for (unsigned i = 0; i < sizeof or_only / sizeof *or_only; i++)
                lv_obj_add_flag(or_only[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_t *body[] = { cost_big, cost_tok, cost_tok_unit, cost_30, cost_cap, cost_chart };
            for (unsigned i = 0; i < sizeof body / sizeof *body; i++)
                lv_obj_clear_flag(body[i], LV_OBJ_FLAG_HIDDEN);
            char m[16], tk[16], m30[16], tk30[16];
            fmt_money(m, sizeof m, p->cost_today_c);
            if (card_entered) {
                anim_count_up(cost_big, p->cost_today_c, count_cents_cb);
            } else {
                lv_label_set_text(cost_big, m);
            }
            fmt_tokens(tk, sizeof tk, p->tok_today);
            lv_label_set_text(cost_tok, tk);
            lv_label_set_text(cost_tok_unit, is_pi ? "max tokens" : "tokens");
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
            char cmx[16];
            fmt_money(cmx, sizeof cmx, mx);
            lv_label_set_text_fmt(cost_cap, is_pi ? "%d DAY PI SPEND (max): %s" : "%d DAY SPEND (max): %s",
                                  n, cmx);
        }
        return;
    }

    // CARD_LIMITS — works for any provider (uses p/pr/s/sr already on device)
    lv_obj_add_flag(cost_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lim_card, LV_OBJ_FLAG_HIDDEN);
    render_card_hdr(lim_hdr, lim_logo, p->id, "LIMITS");

    char pb[12];
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
    lv_obj_clear_flag(title,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(prov_box, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(title, "BARTENDER");

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
        lv_label_set_text(row_id[i], p->id);

        // Provider logo (CodexBar silhouette), tinted with the provider
        // accent (light grey if the provider has no theme color). Hidden if
        // we have no icon for this provider id.
        const lv_image_dsc_t *ic = provider_icon(p->id);
        if (ic) {
            lv_color_t tc;
            lv_image_set_src(row_icon[i], ic);
            lv_obj_set_style_image_recolor(row_icon[i],
                prov_accent(p->id, &tc) ? tc : lv_color_hex(0xe8eaed), 0);
            lv_obj_clear_flag(row_icon[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(row_icon[i], LV_OBJ_FLAG_HIDDEN);
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
            // Weekly bar under session bar (Claude / Codex only).
            if ((strcmp(p->id, "claude") == 0 || strcmp(p->id, "codex") == 0)
                && p->has_s) {
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
    if (s) st.stats = *s;
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
            if (pi >= 0) {                             // open provider page
                st.nav_provider = pi;
                strlcpy(st.nav_id, st.stats.p[pi].id, sizeof st.nav_id);
                st.nav_card     = st.stats.p[pi].has_cost ? CARD_COST : CARD_LIMITS;
                st.nav_level    = NAV_PAGE;
                st.dirty = true;
            }
        } else if (ev->type == APP_EVT_LONG_PRESS) {
            r = UI_INPUT_PASS;                         // → enter_portal()
        }
        /* SWIPE_LEFT at the root: swallow (CONSUMED, no-op) */
        break;

    case NAV_PAGE:
        if (ev->type == APP_EVT_TAP) {                 // cycle Cost ↔ Limit (Cost only if data present)
            st.nav_card = (st.nav_card == CARD_COST) ? CARD_LIMITS
                : (st.stats.p[st.nav_provider].has_cost ? CARD_COST : CARD_LIMITS);
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
