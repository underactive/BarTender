// firmware/main/ui_render_core.c
//
// Core rendering infrastructure: widget globals, LVGL widget construction
// (build_widgets), the top-level render() dispatch, animation helpers, grid
// overlay, and screen transition functions. Card-page rendering lives in
// ui_render_card.c; summary row rendering in ui_render_summary.c.
// EVERYTHING here runs on ui_task only (build_widgets() once, then render()
// per dirty flag).
#include "ui.h"
#include "ui_internal.h"
#include "provider_icons.h"
#include "provider_colors.h"
#include "boot_splash.h"
#include "icon_lock.h"
#include "led.h"
#include "lvgl.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

// ── Widget globals (created once, mutated only on ui_task) ───────────────────
static lv_obj_t *scr, *title, *status, *prov_box, *summary_top, *boot_img, *lock_badge, *footer_bg;
lv_obj_t *row_id[ROWS], *row_bar[ROWS], *row_val[ROWS], *row_val_s[ROWS], *row_icon[ROWS], *row_bar_w[ROWS];

static void reset_summary_token_animation(void);

// Card widget groups (declared extern in ui_internal.h; defined here).
cost_card_t cost;
lim_card_t  lim;
hero_amount_t cost_hero, lim_hero;

// Card-transition tracking moved to `st` (ui_internal.h):
//   st.prev_nav_level, st.prev_nav_provider, st.prev_nav_card
// Initialized in ui.c, reset in ui_set_stats().

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

void ui_update_grid_overlay(const ui_page_grid_t *g)
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

// Forward declarations for static functions defined later but used earlier.
static void create_card_hdr(lv_obj_t *card, lv_obj_t **hdr_out, lv_obj_t **logo_out);
static hero_amount_t make_hero_amount(lv_obj_t *parent);
static void build_summary_widgets(int W);
static void build_grid_overlay(int W, int H);
static void build_cost_card(int W, int H);
static void build_limits_card(int W, int H);
static lv_obj_t *create_page_bg_logo(lv_obj_t *card);

// Active LVGL screen, or NULL before build_widgets() ran. Lets ui.c's task
// guard the screenshot path without owning the widget globals.
lv_obj_t *ui_active_screen(void)
{
    return scr;
}

bool cursor_sess_refresh_needed(const stats_provider_t *p)
{
    if (strcmp(p->id, "cursor") != 0 || !p->ok || !p->primary.has) return false;
    if (!p->has_cu) return true;
    return !p->cu_sess_ok;
}

void build_widgets(void)
{
    scr = lv_screen_active();
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
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

    // Build order == paint order: summary chrome first, then the grid overlay,
    // then the two full-screen card panels (which must paint on top).
    build_summary_widgets(W);
    build_grid_overlay(W, H);
    // (The old swipe menu/submenu widgets were removed: the summary list is
    // now scrolled directly and provider pages are reached by tapping a row.)
    build_cost_card(W, H);
    build_limits_card(W, H);
}

// Summary list: provider rows + the root-screen chrome (title/status,
// provisioning box, boot splash, lock badge). Created before the card panels
// so those paint on top. Extracted from build_widgets (Fowler audit).
static void build_summary_widgets(int W)
{
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
        // Icons are a fixed 32 px source but grid tiles size the widget to
        // 24 px; CONTAIN rescales the image to fit (and centers it) instead
        // of clipping wide marks (openrouter, kimi) against the widget edge.
        // Recomputed on every lv_image_set_src from the current widget size,
        // so 32 px summary rows stay 1:1 and 24 px grid tiles scale to 0.75x.
        lv_image_set_inner_align(row_icon[i], LV_IMAGE_ALIGN_CONTAIN);
        lv_obj_add_flag(row_icon[i], LV_OBJ_FLAG_HIDDEN);

        row_id[i] = lv_label_create(scr);
        lv_obj_set_style_text_color(row_id[i], lv_color_hex(0xe8eaed), 0);
        lv_obj_set_style_text_font(row_id[i], &lv_font_montserrat_14, 0);
        // Line 1: provider name, right of the icon column (long ids like
        // OPENROUTER no longer clip).
        lv_obj_set_pos(row_id[i], ROW_TXT_X, y + 2);
        lv_obj_set_width(row_id[i], W - ROW_TXT_X - 8);

        // Line 2: bar (spans to just before the % column) + % (right).
        row_bar[i] = lv_bar_create(scr);
        lv_obj_set_size(row_bar[i], val_x - ROW_TXT_X - 8, 7);
        lv_obj_set_pos(row_bar[i], ROW_TXT_X, y + 21);
        lv_bar_set_range(row_bar[i], 0, 100);
        lv_obj_set_style_bg_color(row_bar[i], lv_color_hex(0x3a3a3a), 0);
        lv_obj_set_style_bg_opa(row_bar[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row_bar[i], lv_color_hex(0x30c14e), LV_PART_INDICATOR);
        lv_obj_add_event_cb(row_bar[i], balance_bar_draw_cb, LV_EVENT_DRAW_POST, NULL);
        lv_obj_add_event_cb(row_bar[i], balance_bar_ext_size_cb,
                            LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);

        row_val[i] = lv_label_create(scr);
        lv_obj_set_style_text_color(row_val[i], lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(row_val[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_pad_all(row_val[i], 0, 0);
        lv_obj_set_pos(row_val[i], val_x, y + 15);

        row_val_s[i] = lv_label_create(scr);
        lv_obj_set_style_text_color(row_val_s[i], lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(row_val_s[i], &lv_font_montserrat_10, 0);
        lv_obj_set_style_pad_all(row_val_s[i], 0, 0);
        lv_obj_add_flag(row_val_s[i], LV_OBJ_FLAG_HIDDEN);

        // Weekly bar: 3 px tall, right under the session bar (y+30 + 7 + 2 = y+39).
        row_bar_w[i] = lv_bar_create(scr);
        lv_obj_set_size(row_bar_w[i], val_x - ROW_TXT_X - 8, 3);
        lv_obj_set_pos(row_bar_w[i], ROW_TXT_X, y + 28);
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

    lock_badge = lv_image_create(scr);
    lv_image_set_src(lock_badge, &icon_lock);
    lv_obj_set_style_image_recolor(lock_badge, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_image_recolor_opa(lock_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(lock_badge, 0, 0);
    lv_obj_add_flag(lock_badge, LV_OBJ_FLAG_HIDDEN);

    // Opaque footer background — blocks provider rows that extend past
    // the content area from showing behind the status label.
    footer_bg = lv_obj_create(scr);
    lv_obj_set_style_bg_color(footer_bg, lv_color_hex(0x0b0b0b), 0);
    lv_obj_set_style_bg_opa(footer_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(footer_bg, 0, 0);
    lv_obj_set_style_pad_all(footer_bg, 0, 0);
    lv_obj_clear_flag(footer_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(footer_bg, LV_OBJ_FLAG_HIDDEN);
}

// Dashed debug-grid overlay (hidden unless UI_SHOW_GRID_LINES). Extracted
// from build_widgets (Fowler audit).
static void build_grid_overlay(int W, int H)
{
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
}

// Cost card: full-screen panel; hiding the parent hides children. Extracted
// from build_widgets (Fowler audit).
static void build_cost_card(int W, int H)
{
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
    lv_chart_set_type(cost.chart, LV_CHART_TYPE_BAR);
    lv_chart_set_div_line_count(cost.chart, 0, 0);
    lv_obj_set_style_border_width(cost.chart, 0, 0);
    lv_obj_set_style_bg_opa(cost.chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(cost.chart, 2, 0);
    lv_obj_set_style_pad_column(cost.chart, 2, LV_PART_MAIN);
    lv_obj_set_style_line_width(cost.chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_width(cost.chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(cost.chart, 0, LV_PART_INDICATOR);
    cost.ser = lv_chart_add_series(cost.chart, lv_color_hex(0xe06c4b),
                                   LV_CHART_AXIS_PRIMARY_Y);

    cost.bar = lv_bar_create(cost.card);
    lv_obj_set_size(cost.bar, W - 24, 6);
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
}

// Usage-Limits card: full-screen panel. Extracted from build_widgets (Fowler
// audit).
static void build_limits_card(int W, int H)
{
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

    lim.cap = lv_label_create(lim.card);
    lv_obj_add_flag(lim.cap, LV_OBJ_FLAG_HIDDEN);

    // Dynamic layout: compute all lower-card positions from H so the chart
    // automatically fills every available pixel between the session section
    // and the weekly section.
    const int PAD   = 10;
    const int S_END = 130;

    const int x_bar_y = H - 11;
    const int x_lbl_y = x_bar_y - 4 - 14;

    const int w_rst_y = x_lbl_y - PAD - 14;
    const int w_bar_y = w_rst_y - 4 - 5;
    const int w_big_y = w_bar_y - 4 - 26;
    const int w_lbl_y = w_big_y - 4 - 14;

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
    lv_image_set_scale(*logo_out, 112);
    lv_obj_add_flag(*logo_out, LV_OBJ_FLAG_HIDDEN);

    *hdr_out = lv_label_create(card);
    lv_obj_set_style_text_color(*hdr_out, lv_color_hex(0x9aa0a6), 0);
    lv_obj_set_style_text_font(*hdr_out, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(*hdr_out, 2, 2);
}

void render_lock_badge(bool locked, int x, int y)
{
    if (!lock_badge) return;
    if (!locked) {
        lv_obj_add_flag(lock_badge, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_image_set_src(lock_badge, &icon_lock);
    lv_obj_set_pos(lock_badge, x, y);
    lv_obj_clear_flag(lock_badge, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(lock_badge);
}

void render_page_chrome(lv_obj_t *hdr, lv_obj_t *logo, lv_obj_t *bg_logo,
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
                provider_kind(desc->icon_id) == PK_MOONSHOT ? lv_color_hex(0xffffff)
                : (prov_accent(desc->icon_id, &tc) ? tc : lv_color_hex(0xe8eaed)), 0);
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
            provider_kind(desc->icon_id) == PK_MOONSHOT ? lv_color_hex(0xffffff)
            : (prov_accent(desc->icon_id, &tc) ? tc : lv_color_hex(0xe8eaed)), 0);
    }
    lv_obj_set_pos(bg_logo, card_w - (int)(bg->header.w * 0.78f) - 75, 35);
    lv_image_set_scale(bg_logo, 512);
    lv_obj_clear_flag(bg_logo, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_to_index(bg_logo, 0);
}


static void hide_cards(void)     // hide all card panels (chrome stays)
{
    update_bar_pulse(lim.s_bar, 0.0f, NULL);
    update_bar_pulse(lim.a_bar, 0.0f, NULL);
    update_bar_pulse(lim.w_bar, 0.0f, NULL);
    update_bar_pulse(lim.x_bar, 0.0f, NULL);
    lv_obj_add_flag(cost.card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lim.card,  LV_OBJ_FLAG_HIDDEN);
}

void hide_summary_chrome(void)  // hide title/status/rows before a card
{
    lv_obj_add_flag(title,       LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(status,      LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(prov_box,    LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(summary_top, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(footer_bg,   LV_OBJ_FLAG_HIDDEN);
    reset_summary_token_animation();
    hide_hero_amount(&cost_hero);
    lv_obj_set_style_text_font(cost_hero.num, &font_lemonmilk_48, 0);
    lv_obj_set_style_pad_top(cost_hero.num, -8, 0);
    cost_hero_set_parent(cost.card);
    for (int i = 0; i < ROWS; i++) {
        update_bar_pulse(row_bar[i], 0.0f, NULL);
        update_bar_pulse(row_bar_w[i], 0.0f, NULL);
        lv_obj_add_flag(row_id[i],   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(row_bar[i],  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(row_val[i],  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(row_val_s[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(row_icon[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(row_bar_w[i], LV_OBJ_FLAG_HIDDEN);
    }
}

// ── Animation helpers (ui_task only) ─────────────────────────────────────────

static void bar_opa_cb(void *obj, int32_t opa)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)opa, LV_PART_INDICATOR);
}

static void bar_col_pulse_cb(void *obj, int32_t mix)
{
    lv_obj_t *bar = (lv_obj_t *)obj;
    lv_color_t c = lv_color_mix(lv_color_hex(BAR_PULSE_WHITE_HEX),
                                lv_color_hex(BAR_PULSE_GREY_HEX),
                                (uint8_t)mix);
    lv_obj_set_style_bg_color(bar, c, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
}

static bool bar_pulse_running(lv_obj_t *bar)
{
    return lv_anim_get(bar, bar_opa_cb) != NULL
        || lv_anim_get(bar, bar_col_pulse_cb) != NULL;
}

static void bar_pulse_stop(lv_obj_t *bar)
{
    lv_anim_delete(bar, bar_opa_cb);
    lv_anim_delete(bar, bar_col_pulse_cb);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
}

void update_bar_pulse(lv_obj_t *bar, float pct, const char *provider_id)
{
    bool should  = bar_should_pulse(pct);
    bool color   = should && bar_pulse_uses_color_cycle(provider_id);
    bool opa_run = lv_anim_get(bar, bar_opa_cb) != NULL;
    bool col_run = lv_anim_get(bar, bar_col_pulse_cb) != NULL;
    if (should && !bar_pulse_running(bar)) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, bar);
        if (color) {
            lv_anim_set_exec_cb(&a, bar_col_pulse_cb);
            lv_anim_set_values(&a, LV_OPA_40, LV_OPA_COVER);
        } else {
            lv_anim_set_exec_cb(&a, bar_opa_cb);
            lv_anim_set_values(&a, LV_OPA_40, LV_OPA_COVER);
        }
        lv_anim_set_duration(&a, BAR_PULSE_ANIM_MS);
        lv_anim_set_playback_duration(&a, BAR_PULSE_ANIM_MS);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    } else if (should && ((color && opa_run) || (!color && col_run))) {
        // Mode switched (e.g. provider id now available) — restart once.
        bar_pulse_stop(bar);
        update_bar_pulse(bar, pct, provider_id);
    } else if (!should && bar_pulse_running(bar)) {
        bar_pulse_stop(bar);
    }
}

static void cursor_icon_opa_cb(void *obj, int32_t opa)
{
    lv_obj_set_style_image_recolor_opa((lv_obj_t *)obj, (lv_opa_t)opa, 0);
}

void update_cursor_sess_pulse(lv_obj_t *icon, bool needs)
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

void count_pct_cb(void *obj, int32_t v)
{
    lv_label_set_text_fmt((lv_obj_t *)obj, "%d.%d%%", (int)(v / 10), (int)(v % 10));
}

void count_cents_cb(void *obj, int32_t v)
{
    lv_label_set_text_fmt((lv_obj_t *)obj, "$%d.%02d", (int)(v / 100), (int)(v % 100));
}

static int64_t summary_tok_anim_start;
static int64_t summary_tok_anim_target;
static int64_t summary_tok_anim_displayed;
static bool summary_tok_anim_initialized;

static void summary_tok_anim_cb(void *obj, int32_t progress)
{
    int64_t delta = summary_tok_anim_target - summary_tok_anim_start;
    summary_tok_anim_displayed = summary_tok_anim_start +
        (delta * progress) / 1000;
    char buf[32];
    fmt_tokens_full(buf, sizeof buf, summary_tok_anim_displayed);
    lv_label_set_text((lv_obj_t *)obj, buf);
}

void anim_summary_tokens(lv_obj_t *lbl, int64_t start, int64_t target)
{
    lv_anim_delete(lbl, summary_tok_anim_cb);
    summary_tok_anim_start = start;
    summary_tok_anim_target = target;
    summary_tok_anim_displayed = start;
    summary_tok_anim_initialized = true;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, lbl);
    lv_anim_set_exec_cb(&a, summary_tok_anim_cb);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_duration(&a, HERO_COUNTUP_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void reset_summary_token_animation(void)
{
    lv_anim_delete(cost_hero.num, summary_tok_anim_cb);
    summary_tok_anim_initialized = false;
}

void anim_count_up(lv_obj_t *lbl, int32_t target, lv_anim_exec_xcb_t cb)
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

void anim_chart_fadein(lv_obj_t *chart)
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

void set_bar(lv_obj_t *bar, bool has, float v, const stats_provider_t *p)
{
    int iv = clampi((int)(v + 0.5f), 0, 100);
    lv_bar_set_value(bar, has ? bar_fill(iv) : 0, LV_ANIM_ON);
    if (!has || !bar_should_pulse(v) || !bar_pulse_uses_color_cycle(p->id)) {
        lv_obj_set_style_bg_color(bar, bar_color(p, v), LV_PART_INDICATOR);
    }
    update_bar_pulse(bar, has ? v : 0.0f, p->id);
}

void set_bar_used(lv_obj_t *bar, bool has, float v, const stats_provider_t *p)
{
    int iv = clampi((int)(v + 0.5f), 0, 100);
    lv_bar_set_value(bar, has ? iv : 0, LV_ANIM_ON);
    if (!has || !bar_should_pulse(v) || !bar_pulse_uses_color_cycle(p->id)) {
        lv_obj_set_style_bg_color(bar, bar_color(p, v), LV_PART_INDICATOR);
    }
    update_bar_pulse(bar, has ? v : 0.0f, p->id);
}

int32_t render_cost_bar_chart(lv_obj_t *chart, lv_chart_series_t *ser,
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

void set_reset_lbl(lv_obj_t *lbl, const char *ts)
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
void set_hero_amount(hero_amount_t *h, const char *prefix,
                     const char *num, const char *suffix)
{
    lv_label_set_text_fmt(h->num, "%s%s%s",
                          prefix ? prefix : "", num, suffix ? suffix : "");
}

// Set the hero_amount number to a percentage with a "%" suffix (one decimal,
// integer-tenths since LVGL sprintf has no float), or "--" when has==false.
void set_hero_pct(hero_amount_t *h, bool has, float v)
{
    if (!has) { set_hero_amount(h, NULL, "--", NULL); return; }
    int tenths = pct_remaining_tenths(v);
    char nb[16];
    snprintf(nb, sizeof nb, "%d.%d", tenths / 10, tenths % 10);
    set_hero_amount(h, NULL, nb, "%");
}

void set_hero_pct_used(hero_amount_t *h, bool has, float v)
{
    if (!has) { set_hero_amount(h, NULL, "--", NULL); return; }
    int tenths = pct_tenths(true, v);
    char nb[16];
    snprintf(nb, sizeof nb, "%d.%d", tenths / 10, tenths % 10);
    set_hero_amount(h, NULL, nb, "%");
}

// Set the hero_amount number to a "$" + dollars.cents value.
void set_hero_money(hero_amount_t *h, int32_t cents)
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

// Font/offset descriptor for a hero_amount placement.
typedef struct {
    const lv_font_t *cap_font;
    int              cap_dy;
    const lv_font_t *num_font;
    int              num_pad_top;
    int              num_dy;
    int              x_pad;
} hero_style_t;

static void place_hero_styled(hero_amount_t *h, const ui_rect_t *hero,
                              const char *caption, const hero_style_t *s)
{
    lv_obj_clear_flag(h->caption, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(h->caption, caption);
    lv_obj_set_style_text_font(h->caption, s->cap_font, 0);
    lv_obj_set_pos(h->caption, hero->x + s->x_pad, hero->y + s->cap_dy);
    lv_obj_clear_flag(h->num, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_font(h->num, s->num_font, 0);
    lv_obj_set_style_pad_top(h->num, s->num_pad_top, 0);
    lv_obj_set_pos(h->num, hero->x + s->x_pad, hero->y + s->num_dy);
}

void place_hero_amount(hero_amount_t *h, const ui_rect_t *hero, const char *caption)
{
    static const hero_style_t s = {
        .cap_font = &lv_font_montserrat_12, .cap_dy = 2,
        .num_font = &font_lemonmilk_48, .num_pad_top = -8, .num_dy = 28,
        .x_pad = 12,
    };
    place_hero_styled(h, hero, caption, &s);
}

void place_summary_hero_amount(hero_amount_t *h, const ui_rect_t *hero,
                                const char *caption)
{
    static const hero_style_t s = {
        .cap_font = &font_lemonmilk_23, .cap_dy = -8,
        .num_font = &font_lemonmilk_36, .num_pad_top = -6, .num_dy = 22,
        .x_pad = 8,
    };
    place_hero_styled(h, hero, caption, &s);
}

void hide_hero_amount(hero_amount_t *h)
{
    lv_obj_add_flag(h->caption, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(h->num, LV_OBJ_FLAG_HIDDEN);
}

void cost_hero_set_parent(lv_obj_t *parent)
{
    if (lv_obj_get_parent(cost_hero.caption) != parent)
        lv_obj_set_parent(cost_hero.caption, parent);
    if (lv_obj_get_parent(cost_hero.num) != parent)
        lv_obj_set_parent(cost_hero.num, parent);
}

void cost_tok_set_parent(lv_obj_t *card)
{
    if (lv_obj_get_parent(cost.tok) != card)
        lv_obj_set_parent(cost.tok, card);
    if (lv_obj_get_parent(cost.tok_unit) != card)
        lv_obj_set_parent(cost.tok_unit, card);
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
            lv_obj_add_flag(row_val_s[i], LV_OBJ_FLAG_HIDDEN);
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
    if (st.nav_level == NAV_PAGE) {
        int j = -1;
        for (int i = 0; i < st.stats.n && i < STATS_MAX_PROVIDERS; i++)
            if (strcmp(st.stats.p[i].id, st.nav_id) == 0) { j = i; break; }
        if (j < 0) {
            st.nav_level = NAV_SUMMARY;
            led_summary_reset();
        } else {
            st.nav_provider = j;
        }
    }
    if (st.nav_level == NAV_PAGE) {
        if (!st.saver_active || !led_is_transitioning())
            led_set_provider(st.nav_id);

        render_lock_badge(st.locked, s_scr_w - 18, 4);
        if (boot_img) lv_obj_add_flag(boot_img, LV_OBJ_FLAG_HIDDEN);
        render_card();
        return;
    }

    // NAV_SUMMARY — scrollable provider list
    hide_cards();
    st.prev_nav_level = NAV_SUMMARY;   // ensure next card entry re-triggers animations
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
            lv_obj_add_flag(row_val_s[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(row_icon[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(row_bar_w[i], LV_OBJ_FLAG_HIDDEN);
        }
        led_off();
        return;
    }
    lv_obj_add_flag(boot_img, LV_OBJ_FLAG_HIDDEN);

    int const content_y0 = g.content.y + UI_SUMMARY_TOP_ROWS * g.cell_h;

    if (st.fetched_ms > 0) {
        int age = (int)((esp_timer_get_time() / 1000 - st.fetched_ms) / 1000);
        if (age < 0) age = 0;
        char line[128];
        snprintf(line, sizeof line,
                 "%s " LV_SYMBOL_BULLET " updated %ds ago",
                 st.status, age);
        lv_label_set_text(status, line);
    } else {
        lv_label_set_text(status, st.status);
    }

    {
        const ui_rect_t top_r = ui_grid_span(&g, 0, 0, 2, UI_SUMMARY_TOP_ROWS);
        lv_obj_set_pos(summary_top, 0, 0);
        lv_obj_set_size(summary_top, s_scr_w, content_y0);
        if (st.fetched_ms > 0 && st.stats.n > 0) {
            const int64_t total = summary_tok_today_total();
            lv_obj_clear_flag(summary_top, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_opa(summary_top, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(summary_top, lv_color_hex(0x0b0b0b), 0);
            cost_hero_set_parent(scr);
            place_summary_hero_amount(&cost_hero, &top_r, "I/O TOKENS");
            if (!summary_tok_anim_initialized) {
                anim_summary_tokens(cost_hero.num, 0, total);
            } else if (total != summary_tok_anim_target) {
                anim_summary_tokens(cost_hero.num,
                                    summary_tok_anim_displayed, total);
            }
        } else {
            reset_summary_token_animation();
            lv_obj_add_flag(summary_top, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_set_pos(footer_bg, footer_slot.x, footer_slot.y);
    lv_obj_set_size(footer_bg, footer_slot.w, footer_slot.h);
    lv_obj_clear_flag(footer_bg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(status);

    render_lock_badge(st.locked, s_scr_w - 18, 4);
    int count = summary_visible_count();
    // Hide all widget slots first, then show only populated tiles
    for (int slot = 0; slot < ROWS; slot++) {
        lv_obj_add_flag(row_id[slot],   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(row_bar[slot],  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(row_val[slot],  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(row_val_s[slot], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(row_icon[slot], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(row_bar_w[slot], LV_OBJ_FLAG_HIDDEN);
    }
    for (int s = 0; s < SUMMARY_GRID_SLOTS; s++)
        st.summary_grid[s].pi = -1;
    if (count > 0) {
        int slot = 0;
        for (int row = UI_SUMMARY_TOP_ROWS; row < UI_GRID_ROWS; row++) {
            for (int col = 0; col < UI_GRID_COLS; col++) {
                const ui_rect_t cell = ui_grid_span(&g, col, row, 1, 1);
                int pi = summary_provider_at(slot);
                st.summary_grid[slot].cell = cell;
                st.summary_grid[slot].pi = pi;
                if (pi >= 0) {
                    render_grid_tile(slot, &st.stats.p[pi], &cell);
                } else {
                    update_bar_pulse(row_bar[slot], 0.0f, NULL);
                    update_bar_pulse(row_bar_w[slot], 0.0f, NULL);
                    update_cursor_sess_pulse(row_icon[slot], false);
                }
                slot++;
            }
        }
    } else {
        for (int slot = 0; slot < ROWS; slot++) {
            update_bar_pulse(row_bar[slot], 0.0f, NULL);
            update_bar_pulse(row_bar_w[slot], 0.0f, NULL);
            update_cursor_sess_pulse(row_icon[slot], false);
        }
    }
}
