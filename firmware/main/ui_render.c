// firmware/main/ui_render.c
//
// LVGL widget construction + screen layout + draw/update, split out of ui.c.
// EVERYTHING here runs on ui_task only (build_widgets() once, then render() per
// dirty flag). The widget globals and animation-tracking statics are owned by
// this translation unit; shared nav/screensaver state lives in `st` (ui.c).
#include "ui.h"
#include "ui_internal.h"
#include "provider_icons.h"
#include "provider_colors.h"
#include "boot_splash.h"
#include "led.h"
#include "lvgl.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

// ── Widget globals (created once, mutated only on ui_task) ───────────────────
static lv_obj_t *scr, *title, *status, *prov_box, *summary_top, *boot_img, *lock_badge;
static lv_obj_t *row_id[ROWS], *row_bar[ROWS], *row_val[ROWS], *row_icon[ROWS], *row_bar_w[ROWS];

// Card widget groups (declared extern in ui_internal.h; defined here).
cost_card_t cost;
lim_card_t  lim;
hero_amount_t cost_hero, lim_hero;

// Animation state — ui_task only, no mutex needed
static nav_level_t s_prev_nav_level    = NAV_SUMMARY;
static int         s_prev_nav_provider = -1;
static card_kind_t s_prev_nav_card     = CARD_COST;
static int         s_prev_row_bar[ROWS];   // last fill value per summary slot; -1 = unset

// Cached screen size (declared extern in ui_internal.h; defined here; set in
// build_widgets, read by ui.c nav helpers off ui_task — NO LVGL call off-task).
int s_scr_w = 240;
int s_scr_h = 320;

static const bool UI_SHOW_GRID_LINES = false;  // flip true for layout debug overlay

static lv_obj_t *grid_h[UI_GRID_ROWS + 1];
static lv_obj_t *grid_v[UI_GRID_COLS - 1];
static lv_point_precise_t grid_h_pts[UI_GRID_ROWS + 1][2];
static lv_point_precise_t grid_v_pts[UI_GRID_COLS - 1][2];

// Point a grid line at [a, b] and apply the debug-overlay visibility. `pts`
// must be the line's persistent backing store (LVGL keeps the pointer, not a
// copy), so callers pass the matching grid_{h,v}_pts[i] slot.
static void set_grid_line(lv_obj_t *line, lv_point_precise_t pts[2],
                          lv_point_precise_t a, lv_point_precise_t b)
{
    pts[0] = a;
    pts[1] = b;
    lv_line_set_points(line, pts, 2);
    if (UI_SHOW_GRID_LINES) {
        lv_obj_clear_flag(line, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(line);
    } else {
        lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ui_update_grid_overlay(const ui_page_grid_t *g)
{
    for (int i = 0; i <= UI_GRID_ROWS; i++) {
        if (!grid_h[i]) continue;
        const int y = g->content.y + i * g->cell_h;
        set_grid_line(grid_h[i], grid_h_pts[i],
                      (lv_point_precise_t){ g->content.x, y },
                      (lv_point_precise_t){ g->content.x + g->content.w, y });
    }
    for (int i = 0; i < UI_GRID_COLS - 1; i++) {
        if (!grid_v[i]) continue;
        const int x = g->content.x + (i + 1) * g->cell_w;
        set_grid_line(grid_v[i], grid_v_pts[i],
                      (lv_point_precise_t){ x, g->content.y },
                      (lv_point_precise_t){ x, g->content.y + g->content.h });
    }
}

// Forward declarations for functions defined later but used earlier.
static void create_card_hdr(lv_obj_t *card, lv_obj_t **hdr_out, lv_obj_t **logo_out);
static hero_amount_t make_hero_amount(lv_obj_t *parent);
static void hide_hero_amount(hero_amount_t *h);
static void cost_hero_set_parent(lv_obj_t *parent);
static lv_obj_t *create_page_bg_logo(lv_obj_t *card);
static void render_page_chrome(lv_obj_t *hdr, lv_obj_t *logo, lv_obj_t *bg_logo,
                               int card_w, const ui_page_chrome_desc_t *desc);
static void render_lock_badge(bool locked, int x, int y);
static void bar_opa_cb(void *obj, int32_t opa);
static void update_bar_pulse(lv_obj_t *bar, float pct);

// Active LVGL screen, or NULL before build_widgets() ran. Lets ui.c's task
// guard the screenshot path without owning the widget globals.
lv_obj_t *ui_active_screen(void)
{
    return scr;
}

static bool cursor_sess_refresh_needed(const stats_provider_t *p)
{
    if (strcmp(p->id, "cursor") != 0 || !p->ok || !p->has_p) return false;
    if (!p->has_cu) return true;
    return !p->cu_sess_ok;
}

void build_widgets(void)
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
    lv_obj_set_style_text_font(status, &lv_font_montserrat_10, 0);
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

    summary_top = lv_obj_create(scr);
    lv_obj_set_style_bg_opa(summary_top, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(summary_top, 0, 0);
    lv_obj_set_style_pad_all(summary_top, 0, 0);
    lv_obj_clear_flag(summary_top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(summary_top, LV_OBJ_FLAG_HIDDEN);

    boot_img = lv_image_create(scr);
    lv_image_set_src(boot_img, &boot_splash_img);
    lv_obj_set_style_image_recolor_opa(boot_img, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(boot_img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(boot_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(boot_img, LV_OBJ_FLAG_HIDDEN);

    lock_badge = lv_label_create(scr);
    lv_obj_set_style_text_color(lock_badge, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lock_badge, &lv_font_montserrat_12, 0);
    lv_obj_set_style_pad_all(lock_badge, 0, 0);
    lv_obj_add_flag(lock_badge, LV_OBJ_FLAG_HIDDEN);

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
    cost.card = lv_obj_create(scr);
    lv_obj_set_size(cost.card, W, H);
    lv_obj_set_pos(cost.card, 0, 0);
    lv_obj_set_style_bg_color(cost.card, lv_color_hex(0x0b0b0b), 0);
    lv_obj_set_style_bg_opa(cost.card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cost.card, 0, 0);
    lv_obj_set_style_radius(cost.card, 0, 0);
    // The default theme pads lv_obj containers; zero it so child
    // lv_obj_set_pos() coordinates are screen-absolute (otherwise children
    // are offset and a W-24 child overflows the padded content box → "no
    // margin / runs off the right edge").
    lv_obj_set_style_pad_all(cost.card, 0, 0);
    lv_obj_clear_flag(cost.card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cost.card, LV_OBJ_FLAG_HIDDEN);

    cost.bg_logo = create_page_bg_logo(cost.card);
    create_card_hdr(cost.card, &cost.hdr, &cost.logo);

    cost_hero = make_hero_amount(cost.card);

    cost.tok = lv_label_create(cost.card);
    lv_obj_set_style_text_color(cost.tok, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(cost.tok, &font_lemonmilk_24, 0);
    lv_obj_set_pos(cost.tok, 12, 104);

    cost.tok_unit = lv_label_create(cost.card);
    lv_obj_set_style_text_color(cost.tok_unit, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(cost.tok_unit, &lv_font_montserrat_14, 0);
    lv_label_set_text(cost.tok_unit, "tokens");

    cost.cost_30 = lv_label_create(cost.card);
    lv_obj_set_style_text_color(cost.cost_30, lv_color_hex(0xe8eaed), 0);
    lv_obj_set_style_text_font(cost.cost_30, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(cost.cost_30, 12, H - 38);

    cost.cap = lv_label_create(cost.card);
    lv_obj_set_style_text_color(cost.cap, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(cost.cap, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(cost.cap, 12, H - 22);

    cost.chart = lv_chart_create(cost.card);
    lv_obj_set_size(cost.chart, W - 24, H - 178);
    lv_obj_set_pos(cost.chart, 12, 132);
    // BAR graph of the 30-day daily spend (CodexBar cost cache is
    // day-granular — no hourly $, so this is days, not 24h). point_count is
    // set per-render to the real history length so there are NEVER filler
    // points: LV_CHART_POINT_NONE == INT32_MAX, which a chart clamps to the
    // range max and would draw as a full-height block (the old "giant orange
    // bar" bug).
    lv_chart_set_type(cost.chart, LV_CHART_TYPE_BAR);
    lv_chart_set_div_line_count(cost.chart, 0, 0);
    lv_obj_set_style_border_width(cost.chart, 0, 0);
    lv_obj_set_style_bg_opa(cost.chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(cost.chart, 2, 0);   // keep the bars off the edge
    lv_obj_set_style_pad_column(cost.chart, 2, LV_PART_MAIN);
    lv_obj_set_style_line_width(cost.chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_width(cost.chart, 0, LV_PART_INDICATOR);   // no point dots
    lv_obj_set_style_height(cost.chart, 0, LV_PART_INDICATOR);
    cost.ser = lv_chart_add_series(cost.chart, lv_color_hex(0xe06c4b),
                                   LV_CHART_AXIS_PRIMARY_Y);

    cost.bar = lv_bar_create(cost.card);
    lv_obj_set_size(cost.bar, W - 24, 6);   // half-height (was 12)
    lv_obj_set_pos(cost.bar, 12, H - 56);
    lv_bar_set_range(cost.bar, 0, 100);
    lv_obj_set_style_bg_color(cost.bar, lv_color_hex(0x3a3a3a), 0);
    lv_obj_set_style_bg_opa(cost.bar, LV_OPA_COVER, 0);

    cost.bar_lbl = lv_label_create(cost.card);
    lv_obj_set_style_text_color(cost.bar_lbl, lv_color_hex(0xe8eaed), 0);
    lv_obj_set_style_text_font(cost.bar_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(cost.bar_lbl, 12, H - 38);

    lv_obj_add_flag(cost.bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(cost.bar_lbl, LV_OBJ_FLAG_HIDDEN);

    cost.na = lv_label_create(cost.card);
    lv_obj_set_style_text_color(cost.na, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(cost.na, &lv_font_montserrat_14, 0);
    lv_label_set_text(cost.na, "COST DATA NOT\nAVAILABLE YET");
    lv_obj_set_pos(cost.na, 12, 60);
    lv_obj_add_flag(cost.na, LV_OBJ_FLAG_HIDDEN);

    // OpenRouter balance layout: "BALANCE" sub-label + TODAY + THIS WEEK rows.
    // Shown instead of the Claude/Codex token+chart layout when has_balance.
    cost.or_lbl = lv_label_create(cost.card);
    lv_obj_set_style_text_color(cost.or_lbl, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(cost.or_lbl, &lv_font_montserrat_12, 0);
    lv_label_set_text(cost.or_lbl, "BALANCE");
    lv_obj_set_pos(cost.or_lbl, 12, 88);
    lv_obj_add_flag(cost.or_lbl, LV_OBJ_FLAG_HIDDEN);

    cost.or_row1 = lv_label_create(cost.card);
    lv_obj_set_style_text_color(cost.or_row1, lv_color_hex(0xe8eaed), 0);
    lv_obj_set_style_text_font(cost.or_row1, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(cost.or_row1, 12, 118);
    lv_obj_add_flag(cost.or_row1, LV_OBJ_FLAG_HIDDEN);

    cost.or_row2 = lv_label_create(cost.card);
    lv_obj_set_style_text_color(cost.or_row2, lv_color_hex(0xe8eaed), 0);
    lv_obj_set_style_text_font(cost.or_row2, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(cost.or_row2, 12, 140);
    lv_obj_add_flag(cost.or_row2, LV_OBJ_FLAG_HIDDEN);

    // ---- Usage-Limits card ----
    lim.card = lv_obj_create(scr);
    lv_obj_set_size(lim.card, W, H);
    lv_obj_set_pos(lim.card, 0, 0);
    lv_obj_set_style_bg_color(lim.card, lv_color_hex(0x0b0b0b), 0);
    lv_obj_set_style_bg_opa(lim.card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(lim.card, 0, 0);
    lv_obj_set_style_radius(lim.card, 0, 0);
    lv_obj_set_style_pad_all(lim.card, 0, 0);   // see cost.card note
    lv_obj_clear_flag(lim.card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(lim.card, LV_OBJ_FLAG_HIDDEN);

    lim.bg_logo = create_page_bg_logo(lim.card);
    create_card_hdr(lim.card, &lim.hdr, &lim.logo);

    lim_hero = make_hero_amount(lim.card);
    lim.s_bar = lv_bar_create(lim.card);
    lv_obj_set_size(lim.s_bar, W - 24, 9);
    lv_obj_set_pos(lim.s_bar, 12, 104);
    lv_bar_set_range(lim.s_bar, 0, 100);
    lv_obj_set_style_bg_color(lim.s_bar, lv_color_hex(0x3a3a3a), 0);
    lv_obj_set_style_bg_opa(lim.s_bar, LV_OPA_COVER, 0);
    lim.s_rst = lv_label_create(lim.card);
    lv_obj_set_style_text_color(lim.s_rst, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(lim.s_rst, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lim.s_rst, 12, 116);

    // lim.cap (session-24h caption) permanently hidden — line removed from UI.
    lim.cap = lv_label_create(lim.card);
    lv_obj_add_flag(lim.cap, LV_OBJ_FLAG_HIDDEN);

    // Dynamic layout: compute all lower-card positions from H so the chart
    // automatically fills every available pixel between the session section
    // and the weekly section.
    const int PAD   = 10;   // gap between sections
    const int S_END = 130;  // bottom of lim.s_rst  (y=116 + font_12 ~14px)

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

    lim.chart = lv_chart_create(lim.card);
    lv_obj_set_size(lim.chart, W - 24, chart_h > 8 ? chart_h : 8);
    lv_obj_set_pos(lim.chart, 12, chart_y);
    lv_chart_set_type(lim.chart, LV_CHART_TYPE_LINE | LV_CHART_TYPE_CURVE);
    lv_chart_set_div_line_count(lim.chart, 0, 0);
    lv_obj_set_style_border_width(lim.chart, 0, 0);
    lv_obj_set_style_bg_opa(lim.chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(lim.chart, 2, 0);
    lv_obj_set_style_line_width(lim.chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_width(lim.chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(lim.chart, 0, LV_PART_INDICATOR);
    lim.ser = lv_chart_add_series(lim.chart, lv_color_hex(0x30c14e),
                                  LV_CHART_AXIS_PRIMARY_Y);

    // Auto section: overlaps the chart area — exactly one is visible at a time.
    // render_card() shows chart when pct_hist_n>0, Auto section otherwise.
    const int a_lbl_y = chart_y;
    const int a_big_y = a_lbl_y + 14 + 3;
    const int a_bar_y = a_big_y + 26 + 3;
    const int a_rst_y = a_bar_y + 5 + 3;
    lim.a_lbl = lv_label_create(lim.card);
    lv_obj_set_style_text_color(lim.a_lbl, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(lim.a_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lim.a_lbl, 12, a_lbl_y);
    lim.a_big = lv_label_create(lim.card);
    lv_obj_set_style_text_color(lim.a_big, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lim.a_big, &font_lemonmilk_24, 0);
    lv_obj_set_pos(lim.a_big, 12, a_big_y);
    lim.a_bar = lv_bar_create(lim.card);
    lv_obj_set_size(lim.a_bar, W - 24, 5);
    lv_obj_set_pos(lim.a_bar, 12, a_bar_y);
    lv_bar_set_range(lim.a_bar, 0, 100);
    lv_obj_set_style_bg_color(lim.a_bar, lv_color_hex(0x3a3a3a), 0);
    lv_obj_set_style_bg_opa(lim.a_bar, LV_OPA_COVER, 0);
    lim.a_rst = lv_label_create(lim.card);
    lv_obj_set_style_text_color(lim.a_rst, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(lim.a_rst, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lim.a_rst, 12, a_rst_y);
    lv_obj_add_flag(lim.a_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lim.a_big, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lim.a_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lim.a_rst, LV_OBJ_FLAG_HIDDEN);

    lim.w_lbl = lv_label_create(lim.card);
    lv_obj_set_style_text_color(lim.w_lbl, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(lim.w_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lim.w_lbl, 12, w_lbl_y);
    lim.w_big = lv_label_create(lim.card);
    lv_obj_set_style_text_color(lim.w_big, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lim.w_big, &font_lemonmilk_24, 0);
    lv_obj_set_pos(lim.w_big, 12, w_big_y);
    lim.w_bar = lv_bar_create(lim.card);
    lv_obj_set_size(lim.w_bar, W - 24, 5);
    lv_obj_set_pos(lim.w_bar, 12, w_bar_y);
    lv_bar_set_range(lim.w_bar, 0, 100);
    lv_obj_set_style_bg_color(lim.w_bar, lv_color_hex(0x3a3a3a), 0);
    lv_obj_set_style_bg_opa(lim.w_bar, LV_OPA_COVER, 0);
    lim.w_rst = lv_label_create(lim.card);
    lv_obj_set_style_text_color(lim.w_rst, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(lim.w_rst, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lim.w_rst, 12, w_rst_y);
    lv_obj_add_flag(lim.w_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lim.w_big, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lim.w_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lim.w_rst, LV_OBJ_FLAG_HIDDEN);

    lim.x_lbl = lv_label_create(lim.card);
    lv_obj_set_style_text_color(lim.x_lbl, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(lim.x_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lim.x_lbl, 12, x_lbl_y);
    lim.x_val = lv_label_create(lim.card);
    lv_obj_set_style_text_color(lim.x_val, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(lim.x_val, &lv_font_montserrat_12, 0);
    lv_obj_set_width(lim.x_val, W - 24);
    lv_obj_set_pos(lim.x_val, 12, x_lbl_y);
    lv_obj_set_style_text_align(lim.x_val, LV_TEXT_ALIGN_RIGHT, 0);
    lim.x_bar = lv_bar_create(lim.card);
    lv_obj_set_size(lim.x_bar, W - 24, 5);
    lv_obj_set_pos(lim.x_bar, 12, x_bar_y);
    lv_bar_set_range(lim.x_bar, 0, 100);
    lv_obj_set_style_bg_color(lim.x_bar, lv_color_hex(0x3a3a3a), 0);
    lv_obj_set_style_bg_opa(lim.x_bar, LV_OPA_COVER, 0);

}

// ---- navigation helpers --------------------------------------------------

static lv_obj_t *create_page_bg_logo(lv_obj_t *card)
{
    lv_obj_t *bg = lv_image_create(card);
    lv_obj_add_flag(bg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_pivot(bg, 96, 0);
    lv_obj_set_style_image_opa(bg, LV_OPA_COVER, 0);
    lv_obj_set_style_image_recolor_opa(bg, LV_OPA_COVER, 0);
    return bg;
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

static void render_lock_badge(bool locked, int x, int y)
{
    if (!lock_badge) return;
    if (!locked) {
        lv_obj_add_flag(lock_badge, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(lock_badge, LV_SYMBOL_PAUSE);
    lv_obj_set_pos(lock_badge, x, y);
    lv_obj_clear_flag(lock_badge, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(lock_badge);
}

static void render_page_chrome(lv_obj_t *hdr, lv_obj_t *logo, lv_obj_t *bg_logo,
                               int card_w, const ui_page_chrome_desc_t *desc)
{
    if (hdr) {
        if (desc && desc->subtitle)
            lv_label_set_text_fmt(hdr, "%s  %s", desc->title, desc->subtitle);
        else if (desc)
            lv_label_set_text(hdr, desc->title);
    }
    if (!logo || !desc || !desc->icon_id) {
        if (bg_logo) lv_obj_add_flag(bg_logo, LV_OBJ_FLAG_HIDDEN);
        return;
    }

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

    if (!bg_logo) return;
    const lv_image_dsc_t *bg = provider_background_icon(desc->icon_id);
    if (!bg) {
        lv_obj_add_flag(bg_logo, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_image_set_src(bg_logo, bg);
    if (provider_icon_is_full_color(desc->icon_id)) {
        lv_obj_set_style_image_recolor_opa(bg_logo, LV_OPA_TRANSP, 0);
    } else {
        lv_color_t tc;
        lv_obj_set_style_image_recolor_opa(bg_logo, LV_OPA_40, 0);
        lv_obj_set_style_image_recolor(bg_logo,
            prov_accent(desc->icon_id, &tc) ? tc : lv_color_hex(0xe8eaed), 0);
    }
    lv_obj_set_pos(bg_logo, card_w - (int)(bg->header.w * 0.78f) - 75, 35);
    lv_image_set_scale(bg_logo, 512);
    lv_obj_clear_flag(bg_logo, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_to_index(bg_logo, 0);
}


static void hide_cards(void)     // hide all card panels (chrome stays)
{
    update_bar_pulse(lim.s_bar, 0.0f);
    update_bar_pulse(lim.a_bar, 0.0f);
    update_bar_pulse(lim.w_bar, 0.0f);
    update_bar_pulse(lim.x_bar, 0.0f);
    lv_obj_add_flag(cost.card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lim.card,  LV_OBJ_FLAG_HIDDEN);
}

static void hide_summary_chrome(void)  // hide title/status/rows before a card
{
    lv_obj_add_flag(title,       LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(status,      LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(prov_box,    LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(summary_top, LV_OBJ_FLAG_HIDDEN);
    hide_hero_amount(&cost_hero);
    lv_obj_set_style_text_font(cost_hero.num, &font_lemonmilk_48, 0);
    lv_obj_set_style_pad_top(cost_hero.num, -8, 0);
    cost_hero_set_parent(cost.card);
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
        lv_anim_set_duration(&a, BAR_PULSE_ANIM_MS);
        lv_anim_set_playback_duration(&a, BAR_PULSE_ANIM_MS);
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
        lv_anim_set_duration(&a, CURSOR_PULSE_ANIM_MS);
        lv_anim_set_playback_duration(&a, CURSOR_PULSE_ANIM_MS);
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
    lv_anim_set_duration(&a, HERO_COUNTUP_MS);
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
    lv_anim_set_duration(&a, CHART_FADEIN_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

// ─────────────────────────────────────────────────────────────────────────────

static void set_bar(lv_obj_t *bar, bool has, float v, const stats_provider_t *p)
{
    int iv = clampi((int)(v + 0.5f), 0, 100);
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

    lv_chart_set_point_count(chart, (uint32_t)draw_n);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_series_color(chart, ser, color);
    for (int i = 0; i < draw_n; i++) {
        int32_t v = 0;
        if (i < n && mx > 0)
            v = (int32_t)(((int64_t)hist[i] * 100) / mx);
        lv_chart_set_value_by_id(chart, ser, i, v);
    }
    lv_chart_refresh(chart);
    return mx;
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


// Set the hero_amount number to a value wrapped in an optional prefix symbol
// (e.g. "$" -> "$3.59") and/or suffix symbol (e.g. "%" -> "41%"). The symbols
// render in the same font/size as the number. Pass NULL to omit. For animated
// values, drive h->num with anim_count_up() instead.
static void set_hero_amount(hero_amount_t *h, const char *prefix,
                            const char *num, const char *suffix)
{
    lv_label_set_text_fmt(h->num, "%s%s%s",
                          prefix ? prefix : "", num, suffix ? suffix : "");
}

// Set the hero_amount number to a percentage with a "%" suffix (one decimal,
// integer-tenths since LVGL sprintf has no float), or "--" when has==false.
// The static counterpart to anim_count_up(h->num, .., count_pct_cb).
static void set_hero_pct(hero_amount_t *h, bool has, float v)
{
    if (!has) { set_hero_amount(h, NULL, "--", NULL); return; }
    int tenths = clampi((int)(v * 10.0f + 0.5f), 0, 1000);
    char nb[8];
    snprintf(nb, sizeof nb, "%d.%d", tenths / 10, tenths % 10);
    set_hero_amount(h, NULL, nb, "%");
}

// Set the hero_amount number to a "$" + dollars.cents value. The static
// counterpart to anim_count_up(h->num, cents, count_cents_cb).
static void set_hero_money(hero_amount_t *h, int32_t cents)
{
    if (cents < 0) cents = 0;
    char num[16];
    snprintf(num, sizeof num, "%d.%02d", (int)(cents / 100), (int)(cents % 100));
    set_hero_amount(h, "$", num, NULL);
}

// Create a hero_amount on `parent`: a small caption (montserrat-12, gray) above
// a big number (lemonmilk-48, white, -8 leading trim). Both hidden by default.
// The canonical hero widget; one instance per card. Caption + number are owned
// together so they can't be positioned inconsistently.
static hero_amount_t make_hero_amount(lv_obj_t *parent)
{
    hero_amount_t h;
    h.caption = lv_label_create(parent);
    lv_obj_set_style_text_color(h.caption, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(h.caption, &lv_font_montserrat_12, 0);
    lv_obj_add_flag(h.caption, LV_OBJ_FLAG_HIDDEN);
    h.num = lv_label_create(parent);
    lv_obj_set_style_text_color(h.num, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(h.num, &font_lemonmilk_48, 0);
    lv_obj_set_style_pad_top(h.num, -8, 0);
    lv_obj_add_flag(h.num, LV_OBJ_FLAG_HIDDEN);
    return h;
}

// Place a hero_amount as a unit: caption at hero.y + 2, number at hero.y + 28,
// both shown, with the caption text set. The caller then fills the number via
// set_hero_amount() or anim_count_up(h->num, ...). Because this is the only way
// the pair is positioned, the caption can never drift from the number again.
static void place_hero_amount(hero_amount_t *h, const ui_rect_t *hero, const char *caption)
{
    lv_obj_clear_flag(h->caption, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(h->caption, caption);
    lv_obj_set_style_text_font(h->caption, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(h->caption, hero->x + 12, hero->y + 2);
    lv_obj_clear_flag(h->num, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_font(h->num, &font_lemonmilk_48, 0);
    lv_obj_set_style_pad_top(h->num, -8, 0);
    lv_obj_set_pos(h->num, hero->x + 12, hero->y + 28);
}

// Summary hero: lemonmilk-36 + comma glyph — largest size that fits 11-char counts on 240px.
static void place_summary_hero_amount(hero_amount_t *h, const ui_rect_t *hero,
                                        const char *caption)
{
    lv_obj_clear_flag(h->caption, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(h->caption, caption);
    lv_obj_set_style_text_font(h->caption, &font_lemonmilk_23, 0);
    lv_obj_set_pos(h->caption, hero->x + 12, hero->y - 8);
    lv_obj_clear_flag(h->num, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_font(h->num, &font_lemonmilk_36, 0);
    lv_obj_set_style_pad_top(h->num, -6, 0);
    lv_obj_set_pos(h->num, hero->x + 12, hero->y + 22);
}

static void hide_hero_amount(hero_amount_t *h)
{
    lv_obj_add_flag(h->caption, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(h->num, LV_OBJ_FLAG_HIDDEN);
}

// cost_hero lives on cost.card by default; the summary page reparents it onto scr.
static void cost_hero_set_parent(lv_obj_t *parent)
{
    if (lv_obj_get_parent(cost_hero.caption) != parent)
        lv_obj_set_parent(cost_hero.caption, parent);
    if (lv_obj_get_parent(cost_hero.num) != parent)
        lv_obj_set_parent(cost_hero.num, parent);
}

// cost.tok / cost.tok_unit live on cost.card by default; LM Studio STATS
// reparents them onto lim.card while that page is shown.
static void cost_tok_set_parent(lv_obj_t *card)
{
    if (lv_obj_get_parent(cost.tok) != card)
        lv_obj_set_parent(cost.tok, card);
    if (lv_obj_get_parent(cost.tok_unit) != card)
        lv_obj_set_parent(cost.tok_unit, card);
}

// ── render_cost_card helpers ──────────────────────────────────────────────────

// LM Studio TODAY card: tokens hero + requests line + 30-day bar chart.
static void render_lmstudio_chart(const stats_provider_t *p,
                                  const ui_page_grid_t *g,
                                  const ui_rect_t *hero,
                                  bool card_entered)
{
    cost_tok_set_parent(cost.card);
    lv_obj_t *hide[] = { cost.or_lbl, cost.or_row1, cost.or_row2,
                         cost.bar, cost.bar_lbl, cost.cap };
    for (unsigned i = 0; i < sizeof hide / sizeof *hide; i++)
        lv_obj_add_flag(hide[i], LV_OBJ_FLAG_HIDDEN);
    place_hero_amount(&cost_hero, hero, "TOKENS");
    lv_obj_t *show[] = { cost.tok, cost.tok_unit, cost.cost_30, cost.chart };
    for (unsigned i = 0; i < sizeof show / sizeof *show; i++)
        lv_obj_clear_flag(show[i], LV_OBJ_FLAG_HIDDEN);
    // Chart: grid rows 4..8 (1-indexed); 30-day max on row 9 (below 8-row grid).
    const ui_rect_t chart_r = ui_grid_span(g, 0, 3, 2, 5);
    const ui_rect_t footer_r = ui_grid_span(g, 0, 8, 2, 1);
    lv_obj_set_size(cost.chart, chart_r.w - 24, chart_r.h - 8);
    lv_obj_set_pos(cost.chart, chart_r.x + 12, chart_r.y + 4);
    lv_obj_set_pos(cost.cost_30, footer_r.x + 12, footer_r.y + 2);
    lv_obj_set_pos(cost.cap, footer_r.x + 12, footer_r.y + 18);
    char tk[16], rq[16], tk30[16], rq30[16];
    fmt_tokens(tk, sizeof tk, p->lm_tok_today);
    snprintf(rq, sizeof rq, "%d", (int)p->lm_req_today);
    set_hero_amount(&cost_hero, NULL, tk, NULL);   // plain token count, no affix
    lv_label_set_text(cost.tok, rq);
    lv_label_set_text(cost.tok_unit, "requests");
    lv_obj_align_to(cost.tok_unit, cost.tok, LV_ALIGN_OUT_RIGHT_BOTTOM, 5, 0);
    fmt_tokens(tk30, sizeof tk30, p->lm_tok_month_max);
    snprintf(rq30, sizeof rq30, "%d", (int)p->lm_req_month_max);
    lv_label_set_text_fmt(cost.cost_30, "30 DAY MAX: %s Toks " LV_SYMBOL_BULLET "  %s Reqs", tk30, rq30);
    int n = p->lm_ht_n;
    if (n > NAV_HIST_PTS) n = NAV_HIST_PTS;
    lv_color_t cc;
    int32_t ht32[STATS_HIST_MAX];
    i64_hist_to_i32(ht32, p->lm_ht, n);
    (void)render_cost_bar_chart(cost.chart, cost.ser, ht32, n,
        prov_accent(p->id, &cc) ? cc : lv_color_hex(UI_DEFAULT_CHART_COLOR));
    if (card_entered) anim_chart_fadein(cost.chart);
}

// Cursor TODAY card: token hero + 30-day bar chart (no requests, no $, no OR rows).
static void render_cursor_chart(const stats_provider_t *p,
                                const ui_page_grid_t *g,
                                const ui_rect_t *hero,
                                bool card_entered)
{
    lv_obj_t *hide[] = { cost.or_lbl, cost.or_row1, cost.or_row2,
                         cost.bar, cost.bar_lbl, cost.cap,
                         cost.tok, cost.tok_unit };
    for (unsigned i = 0; i < sizeof hide / sizeof *hide; i++)
        lv_obj_add_flag(hide[i], LV_OBJ_FLAG_HIDDEN);
    place_hero_amount(&cost_hero, hero, "TOKENS");
    lv_obj_t *show[] = { cost.cost_30, cost.chart };
    for (unsigned i = 0; i < sizeof show / sizeof *show; i++)
        lv_obj_clear_flag(show[i], LV_OBJ_FLAG_HIDDEN);
    // Chart: grid rows 4..8; 30-day max on row 9 (below 8-row grid).
    const ui_rect_t chart_r = ui_grid_span(g, 0, 3, 2, 5);
    const ui_rect_t footer_r = ui_grid_span(g, 0, 8, 2, 1);
    lv_obj_set_size(cost.chart, chart_r.w - 24, chart_r.h - 8);
    lv_obj_set_pos(cost.chart, chart_r.x + 12, chart_r.y + 4);
    lv_obj_set_pos(cost.cost_30, footer_r.x + 12, footer_r.y + 2);
    char tk[16], tk30[16];
    fmt_tokens(tk, sizeof tk, p->cu_tok_today);
    set_hero_amount(&cost_hero, NULL, tk, NULL);
    fmt_tokens(tk30, sizeof tk30, p->cu_tok_month_max);
    lv_label_set_text_fmt(cost.cost_30, "30 DAY MAX: %s Toks", tk30);
    int n = p->cu_ht_n;
    if (n > NAV_HIST_PTS) n = NAV_HIST_PTS;
    lv_color_t cc;
    int32_t ht32[STATS_HIST_MAX];
    i64_hist_to_i32(ht32, p->cu_ht, n);
    (void)render_cost_bar_chart(cost.chart, cost.ser, ht32, n,
        prov_accent(p->id, &cc) ? cc : lv_color_hex(UI_DEFAULT_CHART_COLOR));
    if (card_entered) anim_chart_fadein(cost.chart);
}

// OpenRouter TODAY card: spend hero + this-week secondary + balance footer.
static void render_cost_openrouter(const stats_provider_t *p,
                                   const ui_page_grid_t *g,
                                   const ui_rect_t *hero,
                                   bool card_entered)
{
    lv_obj_t *or_unused[] = { cost.cap, cost.chart, cost.bar, cost.bar_lbl,
                              cost.cost_30, cost.or_lbl };
    for (unsigned i = 0; i < sizeof or_unused / sizeof *or_unused; i++)
        lv_obj_add_flag(or_unused[i], LV_OBJ_FLAG_HIDDEN);
    const ui_rect_t week_r = ui_grid_span(g, 0, 2, 2, 1);
    const ui_rect_t bal_r  = ui_grid_span(g, 0, 7, 2, 1);
    char bal[16], wk[16];
    place_hero_amount(&cost_hero, hero, "SPEND");
    if (card_entered) {
        anim_count_up(cost_hero.num, p->cost_today_c, count_cents_cb);
    } else {
        set_hero_money(&cost_hero, p->cost_today_c);
    }
    fmt_money(wk, sizeof wk, p->cost_week_c);
    lv_obj_set_pos(cost.tok, week_r.x + 12, week_r.y + 2);
    lv_label_set_text(cost.tok, wk);
    lv_obj_clear_flag(cost.tok, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(cost.tok_unit, "this week");
    lv_obj_align_to(cost.tok_unit, cost.tok, LV_ALIGN_OUT_RIGHT_BOTTOM, 5, 0);
    lv_obj_clear_flag(cost.tok_unit, LV_OBJ_FLAG_HIDDEN);
    fmt_money(bal, sizeof bal, p->credits_remaining_c);
    lv_obj_set_pos(cost.or_row1, bal_r.x + 12, bal_r.y + 2);
    lv_obj_set_style_text_font(cost.or_row1, &font_lemonmilk_24, 0);
    lv_obj_set_style_text_color(cost.or_row1, lv_color_hex(0x9aa0a6), 0);
    lv_label_set_text(cost.or_row1, bal);
    lv_obj_clear_flag(cost.or_row1, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_font(cost.or_row2, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cost.or_row2, lv_color_hex(0x9aa0a6), 0);
    lv_label_set_text(cost.or_row2, "balance");
    lv_obj_align_to(cost.or_row2, cost.or_row1, LV_ALIGN_OUT_RIGHT_BOTTOM, 5, 0);
    lv_obj_clear_flag(cost.or_row2, LV_OBJ_FLAG_HIDDEN);
}

// Claude / Codex / Pi TODAY card: spend hero + tokens + bar chart.
static void render_cost_standard(const stats_provider_t *p,
                                 const ui_page_grid_t *g,
                                 const ui_rect_t *hero,
                                 const ui_rect_t *body,
                                 const ui_rect_t *footer,
                                 provider_kind_t pk,
                                 bool card_entered)
{
    lv_obj_t *or_only[] = { cost.or_lbl, cost.or_row1, cost.or_row2 };
    for (unsigned i = 0; i < sizeof or_only / sizeof *or_only; i++)
        lv_obj_add_flag(or_only[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *body_widgets[] = { cost.tok, cost.tok_unit, cost.cost_30, cost.chart };
    for (unsigned i = 0; i < sizeof body_widgets / sizeof *body_widgets; i++)
        lv_obj_clear_flag(body_widgets[i], LV_OBJ_FLAG_HIDDEN);
    // Claude, Codex and Pi all show the SPEND hero amount via cost_hero.
    place_hero_amount(&cost_hero, hero, "SPEND");
    bool is_standard = (pk == PK_PI || pk == PK_CLAUDE || pk == PK_CODEX);
    if (is_standard) {
        // Pi / Claude / Codex: no N-day spend cap; 30-day summary on row 9 —
        // the strip below the 8-row grid (chart uses rows 4..8).
        lv_obj_add_flag(cost.cap, LV_OBJ_FLAG_HIDDEN);
        const ui_rect_t footer_r = ui_grid_span(g, 0, 8, 2, 1);
        lv_obj_set_pos(cost.cost_30, footer_r.x + 12, footer_r.y + 2);
    } else {
        lv_obj_clear_flag(cost.cap, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(cost.cap, footer->x + 12, footer->y + 4);
        lv_obj_set_pos(cost.cost_30, footer->x + 12, footer->y + 20);
    }
    if (is_standard) {
        const ui_rect_t chart_r = ui_grid_span(g, 0, 3, 2, 5);
        lv_obj_set_size(cost.chart, chart_r.w - 24, chart_r.h - 8);
        lv_obj_set_pos(cost.chart, chart_r.x + 12, chart_r.y + 4);
    } else {
        lv_obj_set_size(cost.chart, body->w - 24, body->h - 30);
        lv_obj_set_pos(cost.chart, body->x + 12, body->y + 4);
    }
    char tk[16], m30[16], tk30[16];
    // SPEND hero amount ($) — count_cents_cb emits "$N.NN" during the
    // count-up; set_hero_money does the same statically.
    if (card_entered) {
        anim_count_up(cost_hero.num, p->cost_today_c, count_cents_cb);
    } else {
        set_hero_money(&cost_hero, p->cost_today_c);
    }
    fmt_tokens(tk, sizeof tk, p->tok_today);
    lv_label_set_text(cost.tok, tk);
    lv_label_set_text(cost.tok_unit, "tokens");
    lv_obj_align_to(cost.tok_unit, cost.tok, LV_ALIGN_OUT_RIGHT_BOTTOM, 5, 0);
    fmt_money(m30, sizeof m30, p->cost_month_c);
    fmt_tokens(tk30, sizeof tk30, p->tok_month);
    if (pk == PK_PI) {
        lv_label_set_text_fmt(cost.cost_30, "30 DAY MAX: %s  " LV_SYMBOL_BULLET "  %s Toks",
                              m30, tk30);
    } else {
        lv_label_set_text_fmt(cost.cost_30, "30 DAYS TOTAL: %s  " LV_SYMBOL_BULLET "  %s Toks",
                              m30, tk30);
    }
    int n = p->hist_n;
    if (n > NAV_HIST_PTS) n = NAV_HIST_PTS;
    lv_color_t cc;
    int32_t mx = render_cost_bar_chart(cost.chart, cost.ser, p->hist, n,
        prov_accent(p->id, &cc) ? cc : lv_color_hex(0xe06c4b));
    if (card_entered) anim_chart_fadein(cost.chart);
    if (!is_standard) {
        char cmx[16];
        fmt_money(cmx, sizeof cmx, mx);
        lv_label_set_text_fmt(cost.cap, "%d DAY SPEND (max): %s", n, cmx);
    }
}

// Dispatch: render the CARD_COST panel for the current provider.
static void render_cost_card(const stats_provider_t *p,
                             const ui_page_grid_t *g,
                             const ui_rect_t *hero,
                             const ui_rect_t *body,
                             const ui_rect_t *footer,
                             provider_kind_t pk,
                             bool has_balance,
                             bool card_entered)
{
    lv_obj_add_flag(lim.card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(cost.card, LV_OBJ_FLAG_HIDDEN);
    // LIMITS/STATS reparent cost.tok onto lim.card; restore before showing TODAY.
    cost_tok_set_parent(cost.card);
    {
        char up[STATS_ID_MAX];
        up_id(up, sizeof up, p->id);
        render_page_chrome(cost.hdr, cost.logo, cost.bg_logo, s_scr_w,
                           &(ui_page_chrome_desc_t){
            .title = up,
            .subtitle = "TODAY",
            .icon_id = p->id,
        });
    }

    if (!p->has_cost && !p->has_lm && !p->has_cu) {
        lv_obj_clear_flag(cost.na, LV_OBJ_FLAG_HIDDEN);
        hide_hero_amount(&cost_hero);
        lv_obj_t *all[] = { cost.tok, cost.tok_unit, cost.cost_30, cost.cap,
                            cost.chart, cost.bar, cost.bar_lbl,
                            cost.or_lbl, cost.or_row1, cost.or_row2 };
        for (unsigned i = 0; i < sizeof all / sizeof *all; i++)
            lv_obj_add_flag(all[i], LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(cost.na, LV_OBJ_FLAG_HIDDEN);

    // cost_hero (hero_amount) is shown only in the provider branches below;
    // hide the pair by default so it can't leak between providers.
    hide_hero_amount(&cost_hero);

    // Secondary metric line (tokens / requests / balance) — same height on
    // every card. hero.y + 76 centers it in body-row 2 so it clears the
    // grid divider lines above (y=90) and below (y=125). cost.tok_unit is
    // aligned to it per-branch once its text width is known.
    lv_obj_set_pos(cost.tok, hero->x + 12, hero->y + 76);

    switch (pk) {
    case PK_LMSTUDIO:
        render_lmstudio_chart(p, g, hero, card_entered);
        return;
    case PK_CURSOR:
        if (p->has_cu) {
            render_cursor_chart(p, g, hero, card_entered);
            return;
        }
        break;
    default:
        break;
    }

    if (has_balance) {
        render_cost_openrouter(p, g, hero, card_entered);
    } else {
        render_cost_standard(p, g, hero, body, footer, pk, card_entered);
    }
}

// ── render_limits_card helpers ────────────────────────────────────────────────

// LM Studio STATS card: tokens% hero + requests% line + bar.
static void render_lmstudio_stats(const stats_provider_t *p,
                                  const ui_page_grid_t *g,
                                  const ui_rect_t *hero,
                                  const ui_rect_t *body,
                                  const ui_rect_t *footer,
                                  bool card_entered)
{
    {
        char up[STATS_ID_MAX];
        up_id(up, sizeof up, p->id);
        render_page_chrome(lim.hdr, lim.logo, lim.bg_logo, s_scr_w,
                           &(ui_page_chrome_desc_t){
            .title = up,
            .subtitle = "STATS",
            .icon_id = p->id,
        });
    }
    lv_obj_set_pos(lim.s_bar, hero->x + 12, hero->y + 84);
    lv_obj_set_pos(lim.s_rst, hero->x + 12, hero->y + 96);
    lv_obj_set_size(lim.chart, body->w - 24, body->h - 10);
    lv_obj_set_pos(lim.chart, body->x + 12, body->y + 4);
    lv_obj_set_pos(lim.w_rst, footer->x + 12, footer->y + 54);
    lv_obj_add_flag(lim.w_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lim.w_big, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lim.a_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lim.a_big, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lim.a_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lim.a_rst, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lim.chart, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lim.x_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lim.x_val, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lim.x_bar, LV_OBJ_FLAG_HIDDEN);
    place_hero_amount(&lim_hero, hero, "TOKENS");
    if (card_entered && p->has_lm) {
        anim_count_up(lim_hero.num, (int32_t)(p->p * 10.0f + 0.5f), count_pct_cb);
    } else {
        set_hero_pct(&lim_hero, p->has_lm, p->p);
    }
    char pb[12];
    set_bar(lim.s_bar, p->has_lm, p->p, p);
    lv_obj_add_flag(lim.s_rst, LV_OBJ_FLAG_HIDDEN);
    cost_tok_set_parent(lim.card);
    {
        const ui_rect_t req_r = ui_grid_span(g, 0, 7, 2, 1);
        lv_obj_set_pos(cost.tok, req_r.x + 12, req_r.y + 2);
        lv_obj_set_pos(lim.w_bar, req_r.x + 12, req_r.y + req_r.h - 5);
    }
    if (p->has_lm) {
        fmt_pct(pb, sizeof pb, p->has_lm, p->s);
        lv_label_set_text(cost.tok, pb);
        lv_label_set_text(cost.tok_unit, "requests");
        lv_obj_align_to(cost.tok_unit, cost.tok, LV_ALIGN_OUT_RIGHT_BOTTOM, 5, 0);
        lv_obj_clear_flag(cost.tok, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(cost.tok_unit, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lim.w_bar, LV_OBJ_FLAG_HIDDEN);
        set_bar(lim.w_bar, true, p->s, p);
    } else {
        lv_obj_add_flag(cost.tok, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(cost.tok_unit, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lim.w_bar, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(lim.w_rst, LV_OBJ_FLAG_HIDDEN);
}

// Dispatch: render the CARD_LIMITS panel for the current provider.
static void render_limits_card(const stats_provider_t *p,
                               const ui_page_grid_t *g,
                               const ui_rect_t *hero,
                               const ui_rect_t *body,
                               const ui_rect_t *footer,
                               provider_kind_t pk,
                               bool has_balance,
                               bool card_entered)
{
    lv_obj_add_flag(cost.card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lim.card, LV_OBJ_FLAG_HIDDEN);
    cost_tok_set_parent(cost.card);
    lv_obj_add_flag(cost.tok, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(cost.tok_unit, LV_OBJ_FLAG_HIDDEN);
    char pb[12];

    // lim_hero is only used by providers migrated to hero_amount (Pi, LM
    // Studio); hide by default so it can't leak onto the others. Those branches
    // re-show it.
    hide_hero_amount(&lim_hero);

    if (pk == PK_LMSTUDIO) {
        render_lmstudio_stats(p, g, hero, body, footer, card_entered);
        return;
    }

    {
        char up[STATS_ID_MAX];
        up_id(up, sizeof up, p->id);
        render_page_chrome(lim.hdr, lim.logo, lim.bg_logo, s_scr_w,
                           &(ui_page_chrome_desc_t){
            .title = up,
            .subtitle = "LIMITS",
            .icon_id = p->id,
        });
    }
    lv_obj_set_pos(lim.s_bar, hero->x + 12, hero->y + 84);
    lv_obj_set_pos(lim.s_rst, hero->x + 12, hero->y + 96);
    lv_obj_set_size(lim.chart, body->w - 24, body->h - 10);
    lv_obj_set_pos(lim.chart, body->x + 12, body->y + 4);
    {
        const ui_rect_t week_r = ui_grid_span(g, 0, 4, 2, 1);
        lv_obj_set_pos(lim.a_lbl, week_r.x + 12, week_r.y + 4);
        lv_obj_set_pos(lim.a_big, week_r.x + 12, week_r.y + 18);
        lv_obj_set_pos(lim.a_bar, week_r.x + 12, week_r.y + 44);
        lv_obj_set_pos(lim.a_rst, week_r.x + 12, week_r.y + 54);
    }
    lv_obj_set_pos(lim.w_lbl, footer->x + 12, footer->y + 4);
    lv_obj_set_pos(lim.w_big, footer->x + 12, footer->y + 18);
    lv_obj_set_pos(lim.w_bar, footer->x + 12, footer->y + 44);
    lv_obj_set_pos(lim.w_rst, footer->x + 12, footer->y + 54);
    lv_obj_set_pos(lim.x_lbl, footer->x + 12, footer->y + 74);
    lv_obj_set_pos(lim.x_val, footer->x + 12, footer->y + 74);
    lv_obj_set_pos(lim.x_bar, footer->x + 12, footer->y + 90);

    // Primary limit hero (Claude/Codex/Cursor/OpenRouter/Pi): caption + pct via
    // the hero_amount pair. Pi's "SESSION" falls out of the same ternary
    // (no balance, no total tier).
    place_hero_amount(&lim_hero, hero,
                      has_balance ? "API KEY" : (p->has_t ? "TOTAL" : "SESSION"));
    if (card_entered && p->has_p) {
        anim_count_up(lim_hero.num, (int32_t)(p->p * 10.0f + 0.5f), count_pct_cb);
    } else {
        set_hero_pct(&lim_hero, p->has_p, p->p);
    }
    set_bar(lim.s_bar, p->has_p, p->p, p);
    if (has_balance) {
        lv_obj_add_flag(lim.s_rst, LV_OBJ_FLAG_HIDDEN);
    } else {
        set_reset_lbl(lim.s_rst, p->pr);
    }

    // Auto section: occupies chart area when no sparkline data and secondary exists.
    // OpenRouter (has_balance) uses hero + budget only — no secondary tier.
    if (!has_balance && p->pct_hist_n == 0 && p->has_s) {
        lv_obj_clear_flag(lim.a_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lim.a_big, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lim.a_bar, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lim.a_lbl, p->has_t ? "AUTO" : "WEEKLY");
        fmt_pct(pb, sizeof pb, p->has_s, p->s);
        lv_label_set_text(lim.a_big, pb);
        set_bar(lim.a_bar, p->has_s, p->s, p);
        set_reset_lbl(lim.a_rst, p->sr);
    } else {
        lv_obj_add_flag(lim.a_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lim.a_big, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lim.a_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lim.a_rst, LV_OBJ_FLAG_HIDDEN);
    }

    // lim_w: "API" tier for providers with tertiary + no sparkline (Cursor).
    // Claude has pct_hist_n > 0; its secondary is the weekly window, not API.
    if (p->has_t && p->pct_hist_n == 0) {
        lv_obj_clear_flag(lim.w_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lim.w_big, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lim.w_bar, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lim.w_lbl, "API");
        fmt_pct(pb, sizeof pb, p->has_t, p->t);
        lv_label_set_text(lim.w_big, pb);
        set_bar(lim.w_bar, p->has_t, p->t, p);
        set_reset_lbl(lim.w_rst, p->tr);
    } else if (p->has_s && p->pct_hist_n > 0) {
        lv_obj_clear_flag(lim.w_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lim.w_big, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lim.w_bar, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lim.w_lbl, "WEEKLY");
        fmt_pct(pb, sizeof pb, p->has_s, p->s);
        lv_label_set_text(lim.w_big, pb);
        set_bar(lim.w_bar, p->has_s, p->s, p);
        set_reset_lbl(lim.w_rst, p->sr);
    } else {
        lv_obj_add_flag(lim.w_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lim.w_big, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lim.w_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lim.w_rst, LV_OBJ_FLAG_HIDDEN);
    }

    if (p->has_cost && p->extra_limit_c > 0) {
        int xp = extra_pct(p);
        if (has_balance) {
            // OpenRouter budget: same cost.tok + cost.tok_unit pair as TODAY balance line.
            int32_t rem = p->extra_limit_c - p->extra_used_c;
            if (rem < 0) rem = 0;
            char rem_str[16], lim_str[16], bud_unit[32];
            fmt_money(rem_str, sizeof rem_str, rem);
            fmt_money(lim_str, sizeof lim_str, p->extra_limit_c);
            snprintf(bud_unit, sizeof bud_unit, "/ %s budget", lim_str);
            cost_tok_set_parent(lim.card);
            {
                const ui_rect_t bud_r = ui_grid_span(g, 0, 3, 2, 1);
                lv_obj_set_pos(cost.tok, bud_r.x + 12, bud_r.y + 2);
                lv_obj_set_pos(lim.w_bar, bud_r.x + 12, bud_r.y + bud_r.h - 5);
            }
            lv_label_set_text(cost.tok, rem_str);
            lv_label_set_text(cost.tok_unit, bud_unit);
            lv_obj_align_to(cost.tok_unit, cost.tok, LV_ALIGN_OUT_RIGHT_BOTTOM, 5, 0);
            lv_obj_clear_flag(cost.tok, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(cost.tok_unit, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lim.w_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lim.w_big, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lim.w_rst, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lim.w_bar, LV_OBJ_FLAG_HIDDEN);
            set_bar(lim.w_bar, true, (float)xp, p);
            update_bar_pulse(lim.x_bar, 0.0f);
            lv_obj_add_flag(lim.x_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lim.x_val, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lim.x_bar, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(lim.x_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lim.x_val, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lim.x_bar, LV_OBJ_FLAG_HIDDEN);
            char a[16], b[16];
            fmt_money(a, sizeof a, p->extra_used_c);
            fmt_money(b, sizeof b, p->extra_limit_c);
            lv_label_set_text(lim.x_lbl, "EXTRA USAGE");
            lv_label_set_text_fmt(lim.x_val, "%s / %s", a, b);
            lv_bar_set_value(lim.x_bar, bar_fill(xp), LV_ANIM_ON);
            lv_obj_set_style_bg_color(lim.x_bar, bar_color(p, (float)xp),
                                      LV_PART_INDICATOR);
            update_bar_pulse(lim.x_bar, (float)xp);
        }
    } else {
        update_bar_pulse(lim.x_bar, 0.0f);
        lv_obj_add_flag(lim.x_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lim.x_val, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lim.x_bar, LV_OBJ_FLAG_HIDDEN);
    }

    // 24h SESSION usage-% sparkline from `ph` (Claude only; absent elsewhere).
    if (p->pct_hist_n > 0) {
        lv_obj_clear_flag(lim.chart, LV_OBJ_FLAG_HIDDEN);
        int n = p->pct_hist_n;
        if (n > STATS_PCT_HIST_MAX) n = STATS_PCT_HIST_MAX;
        lv_chart_set_point_count(lim.chart, (uint32_t)n);
        lv_chart_set_range(lim.chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
        lv_color_t lc;
        lv_chart_set_series_color(lim.chart, lim.ser,
            prov_accent(p->id, &lc) ? lc : lv_color_hex(0x30c14e));
        for (int i = 0; i < n; i++)
            lv_chart_set_value_by_id(lim.chart, lim.ser, i, p->pct_hist[i]);
        lv_chart_refresh(lim.chart);
        if (card_entered) anim_chart_fadein(lim.chart);
    } else {
        lv_obj_add_flag(lim.chart, LV_OBJ_FLAG_HIDDEN);
    }
}

static void render_card(void)   // ui_task only — dispatcher for the NAV_PAGE card
{
    bool card_entered = (s_prev_nav_level    != NAV_PAGE        ||
                         s_prev_nav_provider != st.nav_provider ||
                         s_prev_nav_card     != st.nav_card);
    s_prev_nav_level    = NAV_PAGE;
    s_prev_nav_provider = st.nav_provider;
    s_prev_nav_card     = st.nav_card;

    hide_summary_chrome();

    // Audit (Frontend§MED): defense-in-depth bounds guard. render() re-resolves
    // nav_provider by identity and bails to NAV_SUMMARY if the provider vanished
    // before calling us, so this should always hold — but guarding keeps a
    // future direct caller from indexing st.stats.p[] out of bounds.
    if (st.nav_provider < 0 || st.nav_provider >= st.stats.n ||
        st.nav_provider >= STATS_MAX_PROVIDERS) {
        st.nav_level = NAV_SUMMARY;
        return;
    }

    const stats_provider_t *p = &st.stats.p[st.nav_provider];
    // Data-driven: any provider with a balance/credits field uses the balance layout.
    // Avoids hardcoding "openrouter" and works for any future provider with the same shape.
    bool has_balance = (p->credits_limit_c > 0 || p->credits_remaining_c > 0);
    provider_kind_t pk = provider_kind(p->id);
    const ui_page_grid_t g = ui_grid_from_height(s_scr_w, s_scr_h);
    const ui_rect_t hero   = ui_grid_span(&g, 0, 0, 2, 2);
    const ui_rect_t body   = ui_grid_span(&g, 0, 2, 2, 4);
    const ui_rect_t footer = ui_grid_span(&g, 0, 6, 2, 2);

    ui_update_grid_overlay(&g);

    if (st.nav_card == CARD_COST) {
        render_cost_card(p, &g, &hero, &body, &footer, pk, has_balance, card_entered);
    } else {
        render_limits_card(p, &g, &hero, &body, &footer, pk, has_balance, card_entered);
    }
}

void render(void)   // ui_task only
{
    if (st.mode == UI_PROVISION) {
        hide_cards();
        if (boot_img) lv_obj_add_flag(boot_img, LV_OBJ_FLAG_HIDDEN);
        if (lock_badge) lv_obj_add_flag(lock_badge, LV_OBJ_FLAG_HIDDEN);
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
        if (j < 0) {
            st.nav_level = NAV_SUMMARY;   // provider gone
            led_summary_reset();
        } else {
            st.nav_provider = j;          // follow reorder by identity
        }
    }
    clamp_scroll();

    if (st.nav_level == NAV_PAGE) {
        // During screensaver transitions, led_transition_tick() drives the
        // interpolation — skip led_set_provider() while a transition is in
        // flight so repeated renders don't restart it.
        if (!st.saver_active || !led_is_transitioning())
            led_set_provider(st.nav_id);

        render_lock_badge(st.locked, s_scr_w - 18, 4);
        if (boot_img) lv_obj_add_flag(boot_img, LV_OBJ_FLAG_HIDDEN);
        render_card();
        return;
    }

    // NAV_SUMMARY — scrollable provider list
    hide_cards();
    s_prev_nav_level = NAV_SUMMARY;   // ensure next card entry re-triggers animations
    lv_obj_add_flag(title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(prov_box, LV_OBJ_FLAG_HIDDEN);

    const ui_page_grid_t g = ui_grid_from_height(s_scr_w, s_scr_h);
    ui_update_grid_overlay(&g);

    const ui_rect_t footer_slot = {
        .x = g.content.x,
        .y = g.content.y + g.content.h,
        .w = g.content.w,
        .h = UI_CHROME_BOTTOM,
    };
    lv_obj_set_pos(status, footer_slot.x + 8, footer_slot.y + 2);
    lv_obj_set_width(status, footer_slot.w - 16);
    lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(status, st.status);

    if (boot_splash_visible_locked()) {
        const ui_rect_t splash_r = ui_grid_span(&g, 0, 0, 2, UI_GRID_ROWS);
        lv_obj_set_pos(boot_img, splash_r.x, splash_r.y);
        lv_obj_set_size(boot_img, splash_r.w, splash_r.h);
        {
            const int iw = (int)boot_splash_img.header.w;
            const int ih = (int)boot_splash_img.header.h;
            int sx = splash_r.w * 256 / iw;
            int sy = splash_r.h * 256 / ih;
            int sc = sx < sy ? sx : sy;
            if (sc < 1) sc = 1;
            lv_image_set_scale(boot_img, (uint16_t)sc);
        }
        lv_obj_clear_flag(boot_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(boot_img);
        lv_obj_add_flag(summary_top, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < ROWS; i++) {
            lv_obj_add_flag(row_id[i],   LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(row_bar[i],  LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(row_val[i],  LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(row_icon[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(row_bar_w[i], LV_OBJ_FLAG_HIDDEN);
        }
        led_off();
        return;
    }
    lv_obj_add_flag(boot_img, LV_OBJ_FLAG_HIDDEN);

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
    // --- Footer Widget (Age Suffix) ---
    if (st.fetched_ms > 0) {
        int age = (int)((esp_timer_get_time() / 1000 - st.fetched_ms) / 1000);
        if (age < 0) age = 0;
        char line[128];  // st.status(<=63) + " • updated <int>s ago" + hint
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

    {
        const ui_rect_t top_r = ui_grid_span(&g, 0, 0, 2, UI_SUMMARY_TOP_ROWS);
        lv_obj_set_pos(summary_top, top_r.x, top_r.y);
        lv_obj_set_size(summary_top, top_r.w, top_r.h);
        if (st.fetched_ms > 0 && st.stats.n > 0) {
            lv_obj_clear_flag(summary_top, LV_OBJ_FLAG_HIDDEN);
            cost_hero_set_parent(scr);
            place_summary_hero_amount(&cost_hero, &top_r, "I/O TOKENS");
            char tk[32];
            fmt_tokens_full(tk, sizeof tk, summary_tok_today_total());
            set_hero_amount(&cost_hero, NULL, tk, NULL);
        } else {
            lv_obj_add_flag(summary_top, LV_OBJ_FLAG_HIDDEN);
        }
    }
    render_lock_badge(st.locked, s_scr_w - 18, 4);
    for (int i = 0; i < ROWS; i++) {
        int pi = summary_provider_at(st.scroll + i); // i = visual slot, pi = stats.p[] index
        if (i >= vis || pi < 0) {
            // Audit (Frontend§MED): stop any infinite pulse animations before
            // hiding the slot. hide_summary_chrome() does this on card entry,
            // but a row that simply scrolls off-screen took the HIDDEN path
            // below and kept its ≥90% bar-pulse / cursor-amber anim running on
            // an invisible widget — wasted timer/CPU churn until reuse.
            update_bar_pulse(row_bar[i],   0.0f);
            update_bar_pulse(row_bar_w[i], 0.0f);
            update_cursor_sess_pulse(row_icon[i], false);
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
            const ui_rect_t r = ui_grid_span(&g, 0, UI_SUMMARY_TOP_ROWS + i, 2, 1);
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
            lv_label_set_text(row_id[i], summary_provider_name(p->id));
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
            int v = clampi((int)(p->p + 0.5f), 0, 100);
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
                // Single-source the pct→"45.3%" formatting via fmt_pct()
                // (integer-tenths; LVGL sprintf has no float) instead of
                // re-deriving tenths inline.
                char pctbuf[12];
                fmt_pct(pctbuf, sizeof pctbuf, true, p->p);
                lv_label_set_text(row_val[i], pctbuf);
            }
            // Secondary bar under the primary session/API bar.
            // Claude/Codex use weekly %, LM Studio uses requests %, and
            // OpenRouter uses the same budget bar shown on its LIMITS page.
            {
                provider_kind_t rpk = provider_kind(p->id);
                if (((rpk == PK_CLAUDE || rpk == PK_CODEX) && p->has_s)
                    || rpk == PK_LMSTUDIO) {
                    int wv = clampi((int)(p->s + 0.5f), 0, 100);
                    lv_bar_set_value(row_bar_w[i], bar_fill(wv), LV_ANIM_ON);
                    lv_obj_set_style_bg_color(row_bar_w[i], bar_color(p, p->s), LV_PART_INDICATOR);
                    update_bar_pulse(row_bar_w[i], p->s);
                    lv_obj_clear_flag(row_bar_w[i], LV_OBJ_FLAG_HIDDEN);
                } else if (rpk == PK_OPENROUTER && p->has_cost && p->extra_limit_c > 0) {
                    int xv = extra_pct(p);
                    lv_bar_set_value(row_bar_w[i], bar_fill(xv), LV_ANIM_ON);
                    lv_obj_set_style_bg_color(row_bar_w[i], bar_color(p, (float)xv), LV_PART_INDICATOR);
                    update_bar_pulse(row_bar_w[i], (float)xv);
                    lv_obj_clear_flag(row_bar_w[i], LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(row_bar_w[i], LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
    }
}
