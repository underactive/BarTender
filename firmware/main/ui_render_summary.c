// firmware/main/ui_render_summary.c
//
// Summary page row rendering functions split out of ui_render.c. Called from
// render() in ui_render_core.c. EVERYTHING here runs on ui_task only.
#include "ui.h"
#include "ui_internal.h"
#include "provider_icons.h"
#include "provider_colors.h"
#include "lvgl.h"
#include <string.h>
#include <stdio.h>

// row_bar user_data packs the balance-gauge segment count (low 16 bits) and the
// number of completed-$100 circles (high 16 bits); 0 for non-balance bars.
#define BALANCE_BAR_SEGS(ud)    ((int)((uintptr_t)(ud) & 0xFFFFu))
#define BALANCE_BAR_CIRCLES(ud) ((int)(((uintptr_t)(ud) >> 16) & 0xFFFFu))
#define BALANCE_BAR_PACK(segs, circles) \
    ((void *)(intptr_t)(((unsigned)(circles) << 16) | ((unsigned)(segs) & 0xFFFFu)))

void balance_bar_draw_cb(lv_event_t *e)
{
    lv_obj_t *bar = lv_event_get_target(e);
    void *ud = lv_obj_get_user_data(bar);
    int segs    = BALANCE_BAR_SEGS(ud);
    int circles = BALANCE_BAR_CIRCLES(ud);
    if (segs <= 1 && circles <= 0) return;

    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t area;
    lv_obj_get_coords(bar, &area);
    int width = lv_area_get_width(&area);

    if (segs > 1) {
        lv_draw_line_dsc_t dsc;
        lv_draw_line_dsc_init(&dsc);
        dsc.color = lv_color_hex(0x0b0b0b);
        dsc.width = 2;
        dsc.p1.y = area.y1;
        dsc.p2.y = area.y2;
        for (int k = 1; k < segs; k++) {
            int x = area.x1 + (int)((int64_t)width * k / segs);
            dsc.p1.x = x;
            dsc.p2.x = x;
            lv_draw_line(layer, &dsc);
        }
    }

    // Filled $100 circles, drawn to the left of the (shrunk) bar in the
    // widget's ext_draw_size margin. Same accent color as the bar fill.
    if (circles > 0) {
        lv_draw_rect_dsc_t cd;
        lv_draw_rect_dsc_init(&cd);
        cd.bg_color = lv_obj_get_style_bg_color(bar, LV_PART_INDICATOR);
        cd.bg_opa = LV_OPA_COVER;
        cd.radius = LV_RADIUS_CIRCLE;
        int top = (area.y1 + area.y2) / 2 - BALANCE_CIRCLE_D / 2;
        int x = area.x1 - BALANCE_CIRCLE_PITCH;
        for (int i = 0; i < circles; i++) {
            lv_area_t ca = { x, top, x + BALANCE_CIRCLE_D - 1,
                             top + BALANCE_CIRCLE_D - 1 };
            lv_draw_rect(layer, &cd, &ca);
            x -= BALANCE_CIRCLE_PITCH;
        }
    }
}

// Grant the balance bar room to draw its $100 circles beyond its own width.
void balance_bar_ext_size_cb(lv_event_t *e)
{
    lv_obj_t *bar = lv_event_get_target(e);
    int circles = BALANCE_BAR_CIRCLES(lv_obj_get_user_data(bar));
    if (circles <= 0) return;
    lv_event_set_ext_draw_size(
        e, circles * BALANCE_CIRCLE_PITCH + BALANCE_CIRCLE_GAP + 2);
}

// Providers whose hero metric is the secondary tier (s) rather than the primary
// session window (p). OpenCode Go has 3 tiers and the publisher marks secondary
// as the hero. Qwen publishes a weekly tier only, so without this its absent
// primary drops the tile through to a grey "--".
static bool secondary_is_hero(provider_kind_t k)
{
    return k == PK_OPENCODEGO || k == PK_QWENCLOUD;
}

static int summary_pct_int(provider_kind_t k, float source_pct)
{
    if (provider_pct_is_baseline(k)) {
        int pct = (int)(source_pct + 0.5f);
        return pct < 0 ? 0 : pct;
    }
    return pct_remaining_int(source_pct);
}

static void fmt_summary_pct(char *buf, size_t n, bool has, float source_pct,
                            provider_kind_t k)
{
    if (provider_pct_is_baseline(k))
        fmt_pct_used(buf, n, has, source_pct);
    else
        fmt_pct(buf, n, has, source_pct);
}

// Returns the secondary bar's source usage percentage (0-100), or -1 if the
// secondary bar is hidden for this provider. Quota-backed values convert to
// remaining headroom; baseline-relative values retain their source ratio.
//
// For OpenCode Go the secondary bar shows the tertiary tier (t/tr) instead of
// the secondary tier (s/sr), because the summary page swaps primary/secondary
// display for this provider (secondary replaces the top bar, tertiary becomes
// the smaller bottom bar). Qwen has no tertiary tier, so it returns -1 and the
// small bar stays hidden.
static int secondary_pct(const stats_provider_t *p)
{
    provider_kind_t rpk = provider_kind(p->id);
    if (((rpk == PK_CLAUDE || rpk == PK_CODEX) && p->secondary.has)
        || rpk == PK_LMSTUDIO
        || rpk == PK_OLLAMA
        || (rpk == PK_OPENCODEGO && p->tertiary.has)) {
        float pct = (rpk == PK_OPENCODEGO) ? p->tertiary.pct : p->secondary.pct;
        return clampi((int)(pct + 0.5f), 0, 100);
    }
    if (rpk == PK_OPENROUTER && p->has_cost && p->extra_limit_c > 0) {
        return extra_pct(p);
    }
    return -1;
}

// Nudge Montserrat-10 suffix down so its baseline lines up with Montserrat-14 primary.
#define SECONDARY_PCT_Y_OFFSET  3

// Left-align "36%" + "/ 28%" as one tight group (secondary flush after primary).
static void layout_dual_pct_left(lv_obj_t *primary, lv_obj_t *secondary,
                                 const char *pri_txt, const char *sec_txt,
                                 lv_coord_t left_x, lv_coord_t y)
{
    lv_label_set_text(primary, pri_txt);
    lv_label_set_text(secondary, sec_txt);
    lv_obj_set_width(primary, LV_SIZE_CONTENT);
    lv_obj_set_width(secondary, LV_SIZE_CONTENT);
    lv_obj_update_layout(primary);
    lv_obj_update_layout(secondary);

    lv_obj_set_pos(primary, left_x, y);
    lv_obj_set_pos(secondary, left_x + lv_obj_get_width(primary),
                    y + SECONDARY_PCT_Y_OFFSET);
    lv_obj_clear_flag(secondary, LV_OBJ_FLAG_HIDDEN);
}

// Summary-row secondary bar (row_bar_w): Claude/Codex weekly remaining %, LM
// Studio requests %, or OpenRouter budget headroom; hidden otherwise.
// Extracted from render() (Fowler audit).
//
// For OpenCode Go the secondary bar shows the tertiary tier (t/tr) instead of
// the secondary tier (s/sr), because the summary page swaps primary/secondary
// display for this provider.
static void render_summary_secondary_bar(int slot, const stats_provider_t *p)
{
    provider_kind_t rpk = provider_kind(p->id);
    if (((rpk == PK_CLAUDE || rpk == PK_CODEX) && p->secondary.has)
        || rpk == PK_LMSTUDIO
        || rpk == PK_OLLAMA
        || (rpk == PK_OPENCODEGO && p->tertiary.has)) {
        float pct = (rpk == PK_OPENCODEGO) ? p->tertiary.pct : p->secondary.pct;
        int wv = clampi((int)(pct + 0.5f), 0, 100);
        int fill = provider_pct_is_baseline(rpk) ? wv : bar_fill(wv);
        lv_bar_set_value(row_bar_w[slot], fill, LV_ANIM_OFF);
        if (!bar_should_pulse(pct)
            || !bar_pulse_uses_color_cycle(p->id)) {
            lv_obj_set_style_bg_color(row_bar_w[slot],
                bar_color(p, pct), LV_PART_INDICATOR);
        }
        update_bar_pulse(row_bar_w[slot], pct, p->id);
        lv_obj_clear_flag(row_bar_w[slot], LV_OBJ_FLAG_HIDDEN);
    } else if (rpk == PK_OPENROUTER && p->has_cost && p->extra_limit_c > 0) {
        int xv = extra_pct(p);
        lv_bar_set_value(row_bar_w[slot], bar_fill(xv), LV_ANIM_OFF);
        if (!bar_should_pulse((float)xv) || !bar_pulse_uses_color_cycle(p->id)) {
            lv_obj_set_style_bg_color(row_bar_w[slot],
                bar_color(p, (float)xv), LV_PART_INDICATOR);
        }
        update_bar_pulse(row_bar_w[slot], (float)xv, p->id);
        lv_obj_clear_flag(row_bar_w[slot], LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(row_bar_w[slot], LV_OBJ_FLAG_HIDDEN);
    }
}

// Render one visible summary row: provider name + icon, primary session bar +
// %, and the secondary bar, positioned at pixel_y (pixel-level ticker offset).
// `slot` is the provider index in the visible list (NOT a grid position).
// Extracted from render() (Fowler audit).
void render_summary_row(int slot, const stats_provider_t *p,
                        int pixel_y, int W)
{
    const int val_x = W - 52;
    lv_obj_clear_flag(row_id[slot],  LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(row_val[slot], LV_OBJ_FLAG_HIDDEN);
    {
        bool cu_warn = cursor_sess_refresh_needed(p);
        lv_obj_set_size(row_icon[slot], 32, 32);
        lv_obj_set_pos(row_icon[slot], 8, pixel_y + (ROW_H - ROW_ICON_PX) / 2);
        lv_obj_set_pos(row_id[slot], ROW_TXT_X, pixel_y + 2);
        lv_obj_set_width(row_id[slot], W - ROW_TXT_X - 8);
        lv_obj_set_pos(row_bar[slot], ROW_TXT_X, pixel_y + 21);
        lv_obj_set_size(row_bar[slot], val_x - ROW_TXT_X - 8, 5);
        lv_obj_set_pos(row_val[slot], val_x, pixel_y + 15);
        lv_obj_set_pos(row_bar_w[slot], ROW_TXT_X, pixel_y + 28);
        lv_obj_set_size(row_bar_w[slot], val_x - ROW_TXT_X - 8, 2);
        lv_label_set_text(row_id[slot], summary_provider_name(p->id));
        lv_obj_set_style_text_color(row_id[slot],
            cu_warn ? lv_color_hex(CURSOR_SESS_AMBER) : lv_color_hex(0xe8eaed), 0);
    }

    // Provider logo: A8 silhouette (tinted via recolor) or ARGB8888
    // full-color image (no tinting). Hidden if no icon for this id.
    const lv_image_dsc_t *ic = provider_summary_icon(p->id);
    if (ic) {
        lv_image_set_src(row_icon[slot], ic);
        if (provider_icon_is_full_color(p->id)) {
            lv_obj_set_style_image_recolor_opa(
                row_icon[slot], LV_OPA_TRANSP, 0);
        } else {
            lv_color_t tc;
            lv_obj_set_style_image_recolor_opa(
                row_icon[slot], LV_OPA_COVER, 0);
            lv_obj_set_style_image_recolor(row_icon[slot],
                prov_accent(p->id, &tc) ? tc : lv_color_hex(0xe8eaed), 0);
        }
        lv_obj_clear_flag(row_icon[slot], LV_OBJ_FLAG_HIDDEN);
        update_cursor_sess_pulse(row_icon[slot], cursor_sess_refresh_needed(p));
    } else {
        lv_obj_add_flag(row_icon[slot], LV_OBJ_FLAG_HIDDEN);
        update_cursor_sess_pulse(row_icon[slot], false);
    }

    // Top-bar source depends on provider. Quota-backed values render as
    // remaining headroom; Pi/MiMo/LM Studio baseline ratios stay as used %:
    //  - OpenCode Go / Qwen: secondary tier (s) — see secondary_is_hero();
    //    for OpenCode Go tertiary (t) fills the smaller bottom bar.
    //  - MiMo / Pi / LM Studio: today's tokens vs the 30-day daily average
    //    (excluding zero-use days) via provider_avg_bar() — a "today vs your
    //    typical active day" reading that can exceed 100% on a heavy day.
    //  - everyone else: the windowed primary tier (p).
    provider_kind_t rpk_oc = provider_kind(p->id);
    bool oc_swap = secondary_is_hero(rpk_oc);
    float avg_pct = 0.0f;
    bool avg_swap = provider_avg_bar(p, &avg_pct);
    bool top_has = oc_swap ? p->secondary.has
                           : (avg_swap ? true : p->primary.has);
    float top_pct = oc_swap ? p->secondary.pct
                            : (avg_swap ? avg_pct : p->primary.pct);

    if (!p->ok || !top_has) {
        update_bar_pulse(row_bar[slot], 0.0f, NULL);
        lv_obj_add_flag(row_bar[slot], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(row_val[slot], lv_color_hex(0x6b7075), 0);
        lv_label_set_text(row_val[slot], "off");
        lv_obj_add_flag(row_val_s[slot], LV_OBJ_FLAG_HIDDEN);
    } else {
        int v = clampi((int)(top_pct + 0.5f), 0, 100);
        int fill = provider_pct_is_baseline(rpk_oc) ? v : bar_fill(v);
        lv_obj_clear_flag(row_bar[slot], LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(row_bar[slot], fill, LV_ANIM_OFF);
        if (!bar_should_pulse(top_pct)
            || !bar_pulse_uses_color_cycle(p->id)) {
            lv_obj_set_style_bg_color(row_bar[slot],
                bar_color(p, top_pct), LV_PART_INDICATOR);
        }
        update_bar_pulse(row_bar[slot], top_pct, p->id);
        lv_obj_set_style_text_color(row_val[slot], lv_color_hex(0xffffff), 0);
        {
            int sv = secondary_pct(p);
            if (sv >= 0) {
                char primary_buf[12];
                char secondary_buf[16];
                snprintf(primary_buf, sizeof primary_buf, "%d%%",
                         summary_pct_int(rpk_oc, top_pct));
                snprintf(secondary_buf, sizeof secondary_buf, " / %d%%",
                         summary_pct_int(rpk_oc, (float)sv));
                layout_dual_pct_left(row_val[slot], row_val_s[slot],
                    primary_buf, secondary_buf, (lv_coord_t)val_x,
                    (lv_coord_t)(pixel_y + 15));
            } else {
                char pctbuf[12];
                fmt_summary_pct(pctbuf, sizeof pctbuf, true, top_pct, rpk_oc);
                lv_label_set_text(row_val[slot], pctbuf);
                lv_obj_add_flag(row_val_s[slot], LV_OBJ_FLAG_HIDDEN);
            }
        }
        render_summary_secondary_bar(slot, p);
    }
}

// Render one 1x1 grid tile: icon + percentage + primary bar + secondary bar.
// `slot` indexes into the row_*[ROWS] widget arrays (0..ROWS-1).
// `cell` is the precomputed grid cell rect from ui_grid_span().
// Provider name is NOT shown — the percentage label takes its place.
void render_grid_tile(int slot, const stats_provider_t *p,
                      const ui_rect_t *cell)
{
    lv_obj_clear_flag(row_id[slot],  LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(row_val[slot], LV_OBJ_FLAG_HIDDEN);  // percentage now in row_id

    // Compact icon: 24x24, left-aligned in cell, vertically centered
    lv_obj_set_size(row_icon[slot], 24, 24);
    lv_obj_set_pos(row_icon[slot], cell->x + 4,
                   cell->y + (cell->h - 24) / 2);

    lv_obj_set_style_text_color(row_id[slot], lv_color_hex(0xe8eaed), 0);
    lv_obj_set_pos(row_id[slot], cell->x + 32, cell->y + 2);

    // Top-bar source depends on provider. Quota-backed values render as
    // remaining headroom; Pi/MiMo/LM Studio baseline ratios stay as used %:
    //  - OpenCode Go / Qwen: secondary tier (s) — see secondary_is_hero();
    //    for OpenCode Go tertiary (t) fills the smaller bottom bar.
    //  - MiMo / Pi / LM Studio: today's tokens vs the 30-day daily average
    //    (excluding zero-use days) via provider_avg_bar() — a "today vs your
    //    typical active day" reading that can exceed 100% on a heavy day.
    //  - everyone else: the windowed primary tier (p).
    provider_kind_t rpk_oc = provider_kind(p->id);
    bool oc_swap = secondary_is_hero(rpk_oc);
    float avg_pct = 0.0f;
    bool avg_swap = provider_avg_bar(p, &avg_pct);
    bool top_has = oc_swap ? p->secondary.has
                           : (avg_swap ? true : p->primary.has);
    float top_pct = oc_swap ? p->secondary.pct
                            : (avg_swap ? avg_pct : p->primary.pct);
    // Independent of top_has: e.g. Codex's weekly (secondary) window can be
    // known even when the 5h primary/session window has no recent data.
    int sv = secondary_pct(p);
    int32_t bal_c = 0;
    const bool bal_mode = p->ok && provider_balance_c(p, &bal_c);

    // A slot can switch between a balance and a percentage tile on rerender.
    lv_obj_set_user_data(row_bar[slot], NULL);
    lv_obj_refresh_ext_draw_size(row_bar[slot]);  // drop any prior circle margin
    lv_bar_set_range(row_bar[slot], 0, 100);

    // Percentage label where the provider name used to be
    if (bal_mode) {
        char money[16];
        fmt_money(money, sizeof money, bal_c);
        lv_label_set_text(row_id[slot], money);
        lv_obj_set_width(row_id[slot], cell->w - 36);
        lv_obj_add_flag(row_val_s[slot], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(row_id[slot], lv_color_hex(0xffffff), 0);
    } else if (p->ok && top_has) {
        if (sv >= 0) {
            char primary_buf[12];
            char secondary_buf[16];
            snprintf(primary_buf, sizeof primary_buf, "%d%%",
                     summary_pct_int(rpk_oc, top_pct));
            snprintf(secondary_buf, sizeof secondary_buf, " / %d%%",
                     summary_pct_int(rpk_oc, (float)sv));
            layout_dual_pct_left(row_id[slot], row_val_s[slot],
                primary_buf, secondary_buf,
                (lv_coord_t)(cell->x + 32), (lv_coord_t)(cell->y + 2));
        } else {
            char pctbuf[12];
            fmt_summary_pct(pctbuf, sizeof pctbuf, true, top_pct, rpk_oc);
            lv_label_set_text(row_id[slot], pctbuf);
            lv_obj_set_width(row_id[slot], cell->w - 36);
            lv_obj_add_flag(row_val_s[slot], LV_OBJ_FLAG_HIDDEN);
        }
    } else if (p->ok && sv >= 0) {
        // No primary/session data, but the secondary (e.g. weekly) tier is
        // known — show its percentage instead of a bare "--".
        char pctbuf[12];
        fmt_summary_pct(pctbuf, sizeof pctbuf, true, (float)sv, rpk_oc);
        lv_label_set_text(row_id[slot], pctbuf);
        lv_obj_set_width(row_id[slot], cell->w - 36);
        lv_obj_add_flag(row_val_s[slot], LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(row_id[slot], "--");
        lv_obj_set_width(row_id[slot], cell->w - 36);
        lv_obj_add_flag(row_val_s[slot], LV_OBJ_FLAG_HIDDEN);
    }

    // Primary bar
    lv_obj_set_pos(row_bar[slot], cell->x + 32, cell->y + 18);
    lv_obj_set_size(row_bar[slot], cell->w - 36, 5);

    // Secondary bar
    lv_obj_set_pos(row_bar_w[slot], cell->x + 32, cell->y + 25);
    lv_obj_set_size(row_bar_w[slot], cell->w - 36, 2);

    // Provider logo: A8 silhouette (tinted via recolor) or ARGB8888
    // full-color image (no tinting). Hidden if no icon for this id.
    const lv_image_dsc_t *ic = provider_summary_icon(p->id);
    if (ic) {
        lv_image_set_src(row_icon[slot], ic);
        if (provider_icon_is_full_color(p->id)) {
            lv_obj_set_style_image_recolor_opa(
                row_icon[slot], LV_OPA_TRANSP, 0);
        } else {
            lv_color_t tc;
            lv_obj_set_style_image_recolor_opa(
                row_icon[slot], LV_OPA_COVER, 0);
            lv_obj_set_style_image_recolor(row_icon[slot],
                provider_kind(p->id) == PK_MOONSHOT ? lv_color_hex(0xffffff)
                : (prov_accent(p->id, &tc) ? tc : lv_color_hex(0xe8eaed)), 0);
        }
        lv_obj_clear_flag(row_icon[slot], LV_OBJ_FLAG_HIDDEN);
        update_cursor_sess_pulse(row_icon[slot], cursor_sess_refresh_needed(p));
    } else {
        lv_obj_add_flag(row_icon[slot], LV_OBJ_FLAG_HIDDEN);
        update_cursor_sess_pulse(row_icon[slot], false);
    }

    if (bal_mode) {
        int bar_w = cell->w - 36;
        int circles = balance_circle_count(bal_c);
        int max_circ = (bar_w - BALANCE_GAUGE_MIN_W) / BALANCE_CIRCLE_PITCH;
        if (max_circ < 0) max_circ = 0;
        if (circles > max_circ) circles = max_circ;
        // Every balance bar reads as a fixed $0-100 gauge split into quarters;
        // completed hundreds beyond the current window are carried by circles.
        int32_t window_c = balance_window_c(bal_c, circles);
        // Circles occupy the left of the tile's bar track, so the shrunk
        // bar starts after them and the pair still ends at the same edge.
        lv_obj_set_pos(row_bar[slot],
            cell->x + 32 + circles * BALANCE_CIRCLE_PITCH, cell->y + 18);
        lv_obj_set_size(row_bar[slot], bar_w - circles * BALANCE_CIRCLE_PITCH, 5);
        lv_bar_set_range(row_bar[slot], 0, BALANCE_SEG_MAX * 100);
        lv_bar_set_value(row_bar[slot],
            balance_bar_units(window_c, BALANCE_SEG_MAX), LV_ANIM_OFF);
        lv_obj_set_user_data(row_bar[slot], BALANCE_BAR_PACK(BALANCE_SEG_MAX, circles));
        lv_obj_set_style_bg_color(row_bar[slot], bar_color(p, 0.0f), LV_PART_INDICATOR);
        lv_obj_refresh_ext_draw_size(row_bar[slot]);
        update_bar_pulse(row_bar[slot], 0.0f, NULL);
        lv_obj_clear_flag(row_bar[slot], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(row_bar_w[slot], LV_OBJ_FLAG_HIDDEN);
    } else if (!p->ok || !top_has) {
        // Primary/session tier has no data — hide only its own bar. The
        // secondary bar (e.g. Codex's weekly %) is independent and must
        // still render below when the provider fetch itself succeeded.
        update_bar_pulse(row_bar[slot], 0.0f, NULL);
        lv_obj_add_flag(row_bar[slot], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(row_id[slot],
            (p->ok && sv >= 0) ? lv_color_hex(0xffffff) : lv_color_hex(0x6b7075), 0);
        if (p->ok) render_summary_secondary_bar(slot, p);
    } else {
        int v = clampi((int)(top_pct + 0.5f), 0, 100);
        int fill = provider_pct_is_baseline(rpk_oc) ? v : bar_fill(v);
        lv_obj_clear_flag(row_bar[slot], LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(row_bar[slot], fill, LV_ANIM_OFF);
        if (!bar_should_pulse(top_pct)
            || !bar_pulse_uses_color_cycle(p->id)) {
            lv_obj_set_style_bg_color(row_bar[slot],
                bar_color(p, top_pct), LV_PART_INDICATOR);
        }
        update_bar_pulse(row_bar[slot], top_pct, p->id);
        lv_obj_set_style_text_color(row_id[slot], lv_color_hex(0xffffff), 0);
        render_summary_secondary_bar(slot, p);
    }
}
