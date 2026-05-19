// firmware/main/ui.c
#include "ui.h"
#include "provider_icons.h"
#include "lvgl.h"
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
static struct {
    ui_mode_t mode;
    char ssid[33], pass[64];
    bool prov_wifi_only;          // UI_PROVISION: add-network vs first-boot copy
    char status[64];
    stats_t stats;
    int64_t fetched_ms;
    bool dirty;
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
static lv_obj_t *row_id[ROWS], *row_bar[ROWS], *row_val[ROWS], *row_icon[ROWS];

// Cost card
static lv_obj_t *cost_card, *cost_hdr, *cost_big, *cost_tok, *cost_30,
                *cost_bar, *cost_bar_lbl, *cost_na, *cost_cap;
static lv_obj_t      *cost_chart;
static lv_chart_series_t *cost_ser;

// Usage-Limits card
static lv_obj_t *lim_card, *lim_hdr,
                *lim_s_lbl, *lim_s_big, *lim_s_bar, *lim_s_rst,
                *lim_w_lbl, *lim_w_bar, *lim_w_rst,
                *lim_x_lbl, *lim_x_bar, *lim_cap;
static lv_obj_t      *lim_chart;
static lv_chart_series_t *lim_ser;

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
    static const struct { const char *id; uint32_t hex; } TBL[] = {
        { "codex",       0x49A3B0 }, { "openai",      0x0F826E },
        { "claude",      0xCC7C5E }, { "cursor",      0x00BFA5 },
        { "opencode",    0x3B82F6 }, { "opencodego",  0x3B82F6 },
        { "alibaba",     0xFF6A00 }, { "factory",     0xFF6B35 },
        { "gemini",      0xAB87EA }, { "antigravity", 0x60BA7E },
        { "copilot",     0xA855F7 }, { "zai",         0xE85A6A },
        { "minimax",     0xFE603C }, { "manus",       0x181818 },
        { "kimi",        0xFE603C }, { "kilo",        0xF27027 },
        { "kiro",        0xFF9900 }, { "vertexai",    0x4285F4 },
        { "augment",     0x6366F1 }, { "jetbrains",   0xFF3399 },
        { "kimik2",      0x4C00FF }, { "moonshot",    0x205DEB },
        { "amp",         0xDC2626 }, { "ollama",      0x202020 },
        { "synthetic",   0x141414 }, { "warp",        0x938BB4 },
        { "openrouter",  0x6F42C1 }, { "elevenlabs",  0xEBEBE6 },
        { "windsurf",    0x34E8BB }, { "perplexity",  0x20B2AA },
        { "mimo",        0xFF6900 }, { "doubao",      0x2D88FF },
        { "abacus",      0x38BDF8 }, { "mistral",     0xFF500F },
        { "deepseek",    0x527DF0 }, { "codebuff",    0x44FF00 },
        { "crof",        0x2EAB94 }, { "venice",      0x3399FF },
        { "commandcode", 0x000000 }, { "stepfun",     0xFF8C00 },
        { "bedrock",     0xFF9900 }, { "grok",        0x10A37F },
        { "groq",        0xF56844 }, { "llmproxy",    0x24B47E },
        { "deepgram",    0x0A121B },
    };
    if (!id) return false;
    for (unsigned i = 0; i < sizeof TBL / sizeof *TBL; i++)
        if (strcmp(id, TBL[i].id) == 0) {
            *out = lv_color_hex(TBL[i].hex);
            return true;
        }
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
#define UI_BAR_INVERT_DEFAULT  true
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

// Pin st.scroll into [0, max(0, n - visible)] (closes the list-shrank race).
static void clamp_scroll(void)
{
    int n   = st.stats.n < 0 ? 0 : st.stats.n;
    int max = n - summary_vis_rows();
    if (max < 0)          max = 0;
    if (st.scroll > max)  st.scroll = max;
    if (st.scroll < 0)    st.scroll = 0;
}

// Tap y -> provider index (accounting for scroll), or -1 on a miss / the
// inter-row gap. MUST mirror the summary render geometry (ROW_Y0/ROW_H).
static int summary_hit_test(int y)
{
    if (y < ROW_Y0) return -1;
    int slot = (y - ROW_Y0) / ROW_H;
    if (slot < 0 || slot >= summary_vis_rows()) return -1;
    if ((y - ROW_Y0) % ROW_H > ROW_H - 8) return -1;   // 8 px inter-row gap
    int idx = st.scroll + slot;
    // stats_model.c:72 already caps st.stats.n at STATS_MAX_PROVIDERS; the
    // explicit array-bound check makes the p[] safety local + future-proof.
    if (idx < 0 || idx >= st.stats.n || idx >= STATS_MAX_PROVIDERS) return -1;
    return idx;
}

// tokens -> "123.2M" / "45.6K" / "789" using ONLY integer math (LVGL/newlib
// nano printf has no %lld and no float; everything here fits int32 after the
// divide: 30-day counts are < ~2e9 so /1000 or /1e6 is safe).
static void fmt_tokens(char *buf, size_t n, int64_t t)
{
    if (t < 0) t = 0;
    if (t >= 1000000) {
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
    const int val_x = W - 52;          // right-anchored % column (line 2)

    title = lv_label_create(scr);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_label_set_text(title, "CODEXBAR");
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
        lv_obj_set_style_bg_color(row_bar[i], lv_color_hex(0x222428), 0);
        lv_obj_set_style_bg_color(row_bar[i], lv_color_hex(0x30c14e), LV_PART_INDICATOR);

        row_val[i] = lv_label_create(scr);
        lv_obj_set_style_text_color(row_val[i], lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(row_val[i], &lv_font_montserrat_14, 0);
        lv_obj_set_pos(row_val[i], val_x, y + 26);

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

    cost_hdr = lv_label_create(cost_card);
    lv_obj_set_style_text_color(cost_hdr, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(cost_hdr, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(cost_hdr, 12, 10);

    cost_big = lv_label_create(cost_card);
    lv_obj_set_style_text_color(cost_big, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(cost_big, &lv_font_montserrat_18, 0);
    lv_obj_set_pos(cost_big, 12, 32);

    cost_tok = lv_label_create(cost_card);
    lv_obj_set_style_text_color(cost_tok, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(cost_tok, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(cost_tok, 12, 58);

    cost_30 = lv_label_create(cost_card);
    lv_obj_set_style_text_color(cost_30, lv_color_hex(0xe8eaed), 0);
    lv_obj_set_style_text_font(cost_30, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(cost_30, 12, 76);

    cost_cap = lv_label_create(cost_card);
    lv_obj_set_style_text_color(cost_cap, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(cost_cap, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(cost_cap, 12, 90);

    cost_chart = lv_chart_create(cost_card);
    lv_obj_set_size(cost_chart, W - 24, 96);
    lv_obj_set_pos(cost_chart, 12, 108);
    // LINE sparkline of the 30-day daily spend (CodexBar cost cache is
    // day-granular — no hourly $, so this is days, not 24h). point_count is
    // set per-render to the real history length so there are NEVER filler
    // points: LV_CHART_POINT_NONE == INT32_MAX, which a chart clamps to the
    // range max and would draw as a full-height block (the old "giant orange
    // bar" bug).
    lv_chart_set_type(cost_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(cost_chart, 0, 0);
    lv_obj_set_style_border_width(cost_chart, 0, 0);
    lv_obj_set_style_bg_opa(cost_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(cost_chart, 2, 0);   // keep the line off the edge
    lv_obj_set_style_line_width(cost_chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_width(cost_chart, 0, LV_PART_INDICATOR);   // no point dots
    lv_obj_set_style_height(cost_chart, 0, LV_PART_INDICATOR);
    cost_ser = lv_chart_add_series(cost_chart, lv_color_hex(0xe06c4b),
                                   LV_CHART_AXIS_PRIMARY_Y);

    cost_bar = lv_bar_create(cost_card);
    lv_obj_set_size(cost_bar, W - 24, 6);   // half-height (was 12)
    lv_obj_set_pos(cost_bar, 12, H - 56);
    lv_bar_set_range(cost_bar, 0, 100);
    lv_obj_set_style_bg_color(cost_bar, lv_color_hex(0x222428), 0);

    cost_bar_lbl = lv_label_create(cost_card);
    lv_obj_set_style_text_color(cost_bar_lbl, lv_color_hex(0xe8eaed), 0);
    lv_obj_set_style_text_font(cost_bar_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(cost_bar_lbl, 12, H - 38);

    cost_na = lv_label_create(cost_card);
    lv_obj_set_style_text_color(cost_na, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(cost_na, &lv_font_montserrat_14, 0);
    lv_label_set_text(cost_na, "COST DATA NOT\nAVAILABLE YET");
    lv_obj_set_pos(cost_na, 12, 60);
    lv_obj_add_flag(cost_na, LV_OBJ_FLAG_HIDDEN);

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

    lim_hdr = lv_label_create(lim_card);
    lv_obj_set_style_text_color(lim_hdr, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(lim_hdr, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(lim_hdr, 12, 10);

    lim_s_lbl = lv_label_create(lim_card);
    lv_obj_set_style_text_color(lim_s_lbl, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(lim_s_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lim_s_lbl, 12, 36);
    lim_s_big = lv_label_create(lim_card);
    lv_obj_set_style_text_color(lim_s_big, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lim_s_big, &lv_font_montserrat_18, 0);
    lv_obj_set_pos(lim_s_big, 12, 52);
    lim_s_bar = lv_bar_create(lim_card);
    lv_obj_set_size(lim_s_bar, W - 24, 6);   // half-height (was 12)
    lv_obj_set_pos(lim_s_bar, 12, 80);
    lv_bar_set_range(lim_s_bar, 0, 100);
    lv_obj_set_style_bg_color(lim_s_bar, lv_color_hex(0x222428), 0);
    lim_s_rst = lv_label_create(lim_card);
    lv_obj_set_style_text_color(lim_s_rst, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(lim_s_rst, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lim_s_rst, 12, 96);

    lim_w_lbl = lv_label_create(lim_card);
    lv_obj_set_style_text_color(lim_w_lbl, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(lim_w_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lim_w_lbl, 12, 122);
    lim_w_bar = lv_bar_create(lim_card);
    lv_obj_set_size(lim_w_bar, W - 24, 6);   // half-height (was 12)
    lv_obj_set_pos(lim_w_bar, 12, 140);
    lv_bar_set_range(lim_w_bar, 0, 100);
    lv_obj_set_style_bg_color(lim_w_bar, lv_color_hex(0x222428), 0);
    lim_w_rst = lv_label_create(lim_card);
    lv_obj_set_style_text_color(lim_w_rst, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(lim_w_rst, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lim_w_rst, 12, 156);

    lim_x_lbl = lv_label_create(lim_card);
    lv_obj_set_style_text_color(lim_x_lbl, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(lim_x_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lim_x_lbl, 12, 182);
    lim_x_bar = lv_bar_create(lim_card);
    lv_obj_set_size(lim_x_bar, W - 24, 6);   // half-height (was 12)
    lv_obj_set_pos(lim_x_bar, 12, 200);
    lv_bar_set_range(lim_x_bar, 0, 100);
    lv_obj_set_style_bg_color(lim_x_bar, lv_color_hex(0x222428), 0);

    // 24h SESSION usage-% sparkline (from payload `ph`; Claude only).
    lim_cap = lv_label_create(lim_card);
    lv_obj_set_style_text_color(lim_cap, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(lim_cap, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lim_cap, 12, 224);

    lim_chart = lv_chart_create(lim_card);
    lv_obj_set_size(lim_chart, W - 24, 64);
    lv_obj_set_pos(lim_chart, 12, 240);
    lv_chart_set_type(lim_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(lim_chart, 0, 0);
    lv_obj_set_style_border_width(lim_chart, 0, 0);
    lv_obj_set_style_bg_opa(lim_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(lim_chart, 2, 0);
    lv_obj_set_style_line_width(lim_chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_width(lim_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(lim_chart, 0, LV_PART_INDICATOR);
    lim_ser = lv_chart_add_series(lim_chart, lv_color_hex(0x30c14e),
                                  LV_CHART_AXIS_PRIMARY_Y);
}

// ---- navigation helpers --------------------------------------------------

static void up_id(char *dst, size_t n, const char *src)
{
    size_t j = 0;
    for (; src && src[j] && j + 1 < n; j++)
        dst[j] = (char)toupper((unsigned char)src[j]);
    dst[j] = '\0';
}

static void hide_cards(void)     // hide the Cost/Limit panels (chrome stays)
{
    lv_obj_add_flag(cost_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lim_card,  LV_OBJ_FLAG_HIDDEN);
}

static void hide_summary_chrome(void)  // hide title/status/rows before a card
{
    lv_obj_add_flag(title,    LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(status,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(prov_box, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < ROWS; i++) {
        lv_obj_add_flag(row_id[i],   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(row_bar[i],  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(row_val[i],  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(row_icon[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void fmt_pct(char *buf, size_t n, bool has, float v)
{
    if (!has) { snprintf(buf, n, "--"); return; }
    if (v > 0.0f && v < 1.0f) {
        int t = (int)(v * 10.0f + 0.5f);          // integer tenths (no %f)
        snprintf(buf, n, "%d.%d%%", t / 10, t % 10);
    } else {
        int iv = (int)(v + 0.5f);
        if (iv < 0) iv = 0; else if (iv > 100) iv = 100;
        snprintf(buf, n, "%d%%", iv);
    }
}

static void set_bar(lv_obj_t *bar, bool has, float v, const stats_provider_t *p)
{
    int iv = (int)(v + 0.5f);
    if (iv < 0) iv = 0; else if (iv > 100) iv = 100;
    lv_bar_set_value(bar, has ? bar_fill(iv) : 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, bar_color(p, v), LV_PART_INDICATOR);
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

static void render_card(void)   // ui_task only (renders the NAV_PAGE card)
{
    hide_summary_chrome();

    const stats_provider_t *p = &st.stats.p[st.nav_provider];
    char up[STATS_ID_MAX];
    up_id(up, sizeof up, p->id);

    if (st.nav_card == CARD_COST) {
        lv_obj_add_flag(lim_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(cost_card, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(cost_hdr, "%s  COST", up);

        lv_obj_t *body[] = { cost_big, cost_tok, cost_30, cost_cap,
                             cost_chart, cost_bar, cost_bar_lbl };
        if (!p->has_cost) {                       // non-Claude / cache miss
            lv_obj_clear_flag(cost_na, LV_OBJ_FLAG_HIDDEN);
            for (unsigned i = 0; i < sizeof body / sizeof *body; i++)
                lv_obj_add_flag(body[i], LV_OBJ_FLAG_HIDDEN);
            return;
        }
        lv_obj_add_flag(cost_na, LV_OBJ_FLAG_HIDDEN);
        for (unsigned i = 0; i < sizeof body / sizeof *body; i++)
            lv_obj_clear_flag(body[i], LV_OBJ_FLAG_HIDDEN);

        char m[16], tk[16], m30[16], tk30[16];
        fmt_money(m, sizeof m, p->cost_today_c);
        lv_label_set_text(cost_big, m);
        fmt_tokens(tk, sizeof tk, p->tok_today);
        lv_label_set_text_fmt(cost_tok, "%s TOKENS TODAY", tk);
        fmt_money(m30, sizeof m30, p->cost_month_c);
        fmt_tokens(tk30, sizeof tk30, p->tok_month);
        lv_label_set_text_fmt(cost_30, "30D  %s  " LV_SYMBOL_BULLET "  %s",
                              m30, tk30);

        // Real 30-day daily-spend line. point_count == the actual history
        // length (NO filler points: LV_CHART_POINT_NONE==INT32_MAX clamps to
        // range-max and draws as a full block). CodexBar's cache is
        // day-granular so this is days, not 24h (exec-plan Decision #3).
        int n = p->hist_n;
        if (n < 1) n = 1;
        if (n > NAV_HIST_PTS) n = NAV_HIST_PTS;
        int32_t mx = 1;
        for (int i = 0; i < n && i < p->hist_n; i++)
            if (p->hist[i] > mx) mx = p->hist[i];
        lv_chart_set_point_count(cost_chart, (uint32_t)n);
        // +1/8 headroom keeps the peak off the top edge; +1 avoids a
        // degenerate 0..0 range when all days are zero.
        lv_chart_set_range(cost_chart, LV_CHART_AXIS_PRIMARY_Y, 0, mx + mx / 8 + 1);
        for (int i = 0; i < n; i++)
            lv_chart_set_value_by_id(cost_chart, cost_ser, i,
                                     (i < p->hist_n) ? p->hist[i] : 0);
        // Series follows the provider's CodexBar brand color.
        lv_color_t cc;
        lv_chart_set_series_color(cost_chart, cost_ser,
            prov_accent(p->id, &cc) ? cc : lv_color_hex(0xe06c4b));
        lv_chart_refresh(cost_chart);
        char cmx[16];
        fmt_money(cmx, sizeof cmx, mx);
        lv_label_set_text_fmt(cost_cap, "%d-DAY SPEND  " LV_SYMBOL_BULLET
                              "  max %s", p->hist_n, cmx);

        int xp = extra_pct(p);
        lv_bar_set_value(cost_bar, bar_fill(xp), LV_ANIM_OFF);
        lv_obj_set_style_bg_color(cost_bar, bar_color(p, (float)xp),
                                  LV_PART_INDICATOR);
        char xu[16], xl[16];
        fmt_money(xu, sizeof xu, p->extra_used_c);
        fmt_money(xl, sizeof xl, p->extra_limit_c);
        lv_label_set_text_fmt(cost_bar_lbl, "EXTRA %s / %s", xu, xl);
        return;
    }

    // CARD_LIMITS — works for any provider (uses p/pr/s/sr already on device)
    lv_obj_add_flag(cost_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lim_card, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(lim_hdr, "%s  LIMITS", up);

    char pb[12];
    lv_label_set_text(lim_s_lbl, "SESSION");
    fmt_pct(pb, sizeof pb, p->has_p, p->p);
    lv_label_set_text(lim_s_big, pb);
    set_bar(lim_s_bar, p->has_p, p->p, p);
    lv_label_set_text(lim_s_rst, p->pr);

    fmt_pct(pb, sizeof pb, p->has_s, p->s);
    lv_label_set_text_fmt(lim_w_lbl, "WEEKLY  %s", pb);
    set_bar(lim_w_bar, p->has_s, p->s, p);
    lv_label_set_text(lim_w_rst, p->sr);

    if (p->has_cost && p->extra_limit_c > 0) {
        char a[16], b[16];
        fmt_money(a, sizeof a, p->extra_used_c);
        fmt_money(b, sizeof b, p->extra_limit_c);
        lv_label_set_text_fmt(lim_x_lbl, "EXTRA USAGE  %s / %s", a, b);
        int xp = extra_pct(p);
        lv_bar_set_value(lim_x_bar, bar_fill(xp), LV_ANIM_OFF);
        lv_obj_set_style_bg_color(lim_x_bar, bar_color(p, (float)xp),
                                  LV_PART_INDICATOR);
    } else {
        lv_label_set_text(lim_x_lbl, "EXTRA USAGE  n/a");
        lv_bar_set_value(lim_x_bar, 0, LV_ANIM_OFF);
    }

    // 24h SESSION usage-% sparkline from `ph` (Claude only; absent elsewhere).
    if (p->pct_hist_n > 0) {
        lv_obj_clear_flag(lim_cap, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lim_chart, LV_OBJ_FLAG_HIDDEN);
        int n = p->pct_hist_n;
        if (n > STATS_PCT_HIST_MAX) n = STATS_PCT_HIST_MAX;
        lv_chart_set_point_count(lim_chart, (uint32_t)n);
        lv_chart_set_range(lim_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
        // Match the provider theme (Claude orange) so the limits page is
        // coherent; default green for un-themed providers.
        lv_color_t lc;
        lv_chart_set_series_color(lim_chart, lim_ser,
            prov_accent(p->id, &lc) ? lc : lv_color_hex(0x30c14e));
        for (int i = 0; i < n; i++)
            lv_chart_set_value_by_id(lim_chart, lim_ser, i, p->pct_hist[i]);
        lv_chart_refresh(lim_chart);
        lv_label_set_text_fmt(lim_cap, "SESSION 24H  " LV_SYMBOL_BULLET
                              "  now %d%%", p->pct_hist[n - 1]);
    } else {
        lv_obj_add_flag(lim_cap, LV_OBJ_FLAG_HIDDEN);
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

    if (st.nav_level == NAV_PAGE) {
        render_card();
        return;
    }

    // NAV_SUMMARY — scrollable provider list
    hide_cards();
    lv_obj_clear_flag(title,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(prov_box, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(title, "CODEXBAR");

    // Scrollable window: `vis` rows fit; st.scroll is the top provider index.
    // ASCII-only " +N more" hint (font ships 0x20-0x7F + 0xB0 + 0x2022 only)
    // when the list is longer than the screen, so it never looks truncated.
    int vis  = summary_vis_rows();
    int more = st.stats.n - (st.scroll + vis);   // rows hidden BELOW the
    if (more < 0) more = 0;                       // current window (Audit§P1-3)
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
        int pi = st.scroll + i;                 // i = visual slot, pi = provider
        if (i >= vis || pi >= st.stats.n) {
            lv_obj_add_flag(row_id[i],   LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(row_bar[i],  LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(row_val[i],  LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(row_icon[i], LV_OBJ_FLAG_HIDDEN);
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
            lv_obj_add_flag(row_bar[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_color(row_val[i], lv_color_hex(0x6b7075), 0);
            lv_label_set_text(row_val[i], "off");
        } else {
            int v = (int)(p->p + 0.5f);
            if (v < 0) v = 0; else if (v > 100) v = 100;
            lv_obj_clear_flag(row_bar[i], LV_OBJ_FLAG_HIDDEN);
            lv_bar_set_value(row_bar[i], bar_fill(v), LV_ANIM_OFF);
            // Per-provider accent (Claude orange); un-themed providers keep
            // the green/amber/red usage ramp.
            lv_obj_set_style_bg_color(row_bar[i], bar_color(p, p->p), LV_PART_INDICATOR);
            lv_obj_set_style_text_color(row_val[i], lv_color_hex(0xffffff), 0);
            if (p->p < 1.0f && p->p > 0.0f) {
                // Audit UI§HIGH: LVGL's built-in sprintf is compiled without
                // float support (CONFIG_LV_USE_FLOAT unset), so "%.1f" renders
                // the literal conversion char -> "f%" on screen. Format one
                // decimal place with integer math. Quotient/remainder (not
                // "0.%d") so a value that rounds up to 10 tenths, e.g. 0.97,
                // shows "1.0%" instead of a broken "0.10%".
                int tenths = (int)(p->p * 10.0f + 0.5f);   // 0.1 -> 1
                lv_label_set_text_fmt(row_val[i], "%d.%d%%",
                                      tenths / 10, tenths % 10);
            } else
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
    st.nav_level = NAV_SUMMARY;
    strlcpy(st.status, "starting...", sizeof st.status);
    st.dirty = true;
    xTaskCreate(ui_task, "ui", 8192, NULL, 5, NULL);
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
                st.nav_card     = CARD_COST;
                st.nav_level    = NAV_PAGE;
                st.dirty = true;
            }
        } else if (ev->type == APP_EVT_LONG_PRESS) {
            r = UI_INPUT_PASS;                         // → enter_portal()
        }
        /* SWIPE_LEFT at the root: swallow (CONSUMED, no-op) */
        break;

    case NAV_PAGE:
        if (ev->type == APP_EVT_TAP) {                 // cycle Cost ↔ Limit
            st.nav_card = (st.nav_card == CARD_COST) ? CARD_LIMITS
                                                     : CARD_COST;
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
