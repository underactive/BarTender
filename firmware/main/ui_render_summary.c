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

// Returns the secondary bar's display percentage (0-100), or -1 if the secondary
// bar is hidden for this provider. Mirrors the side-bar visibility logic so the
// value label can show "36% / 28%" when a secondary bar is present.
//
// For OpenCode Go the secondary bar shows the tertiary tier (t/tr) instead of
// the secondary tier (s/sr), because the summary page swaps primary/secondary
// display for this provider (secondary replaces the top bar, tertiary becomes
// the smaller bottom bar).
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
        return clampi(100 - extra_pct(p), 0, 100);
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

// Summary-row secondary bar (row_bar_w): Claude/Codex weekly %, LM Studio
// requests %, or OpenRouter budget %; hidden otherwise. Extracted from render()
// (Fowler audit).
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
        lv_bar_set_value(row_bar_w[slot], bar_fill(wv), LV_ANIM_OFF);
        if (!bar_should_pulse(pct)
            || !bar_pulse_uses_color_cycle(p->id)) {
            lv_obj_set_style_bg_color(row_bar_w[slot],
                bar_color(p, pct), LV_PART_INDICATOR);
        }
        update_bar_pulse(row_bar_w[slot], pct, p->id);
        lv_obj_clear_flag(row_bar_w[slot], LV_OBJ_FLAG_HIDDEN);
    } else if (rpk == PK_OPENROUTER && p->has_cost && p->extra_limit_c > 0) {
        int xv = extra_pct(p);
        int rem = 100 - xv;
        lv_bar_set_value(row_bar_w[slot], bar_fill(rem), LV_ANIM_OFF);
        if (!bar_should_pulse((float)rem) || !bar_pulse_uses_color_cycle(p->id)) {
            lv_obj_set_style_bg_color(row_bar_w[slot],
                bar_color(p, (float)rem), LV_PART_INDICATOR);
        }
        update_bar_pulse(row_bar_w[slot], (float)rem, p->id);
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

    // For OpenCode Go the top bar shows secondary tier (s) instead of primary (p),
    // because OpenCode Go has 3 tiers and the publisher marks secondary as the
    // most useful hero metric on the summary page. The tertiary tier (t) fills
    // the smaller bottom bar.
    provider_kind_t rpk_oc = provider_kind(p->id);
    bool oc_swap = (rpk_oc == PK_OPENCODEGO);
    bool top_has = oc_swap ? p->secondary.has : p->primary.has;
    float top_pct = oc_swap ? p->secondary.pct : p->primary.pct;

    if (!p->ok || !top_has) {
        update_bar_pulse(row_bar[slot], 0.0f, NULL);
        lv_obj_add_flag(row_bar[slot], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(row_val[slot], lv_color_hex(0x6b7075), 0);
        lv_label_set_text(row_val[slot], "off");
        lv_obj_add_flag(row_val_s[slot], LV_OBJ_FLAG_HIDDEN);
    } else {
        int v = clampi((int)(top_pct + 0.5f), 0, 100);
        int fill = bar_fill(v);
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
                snprintf(primary_buf, sizeof primary_buf, "%d%%", v);
                snprintf(secondary_buf, sizeof secondary_buf, " / %d%%", sv);
                layout_dual_pct_left(row_val[slot], row_val_s[slot],
                    primary_buf, secondary_buf, (lv_coord_t)val_x,
                    (lv_coord_t)(pixel_y + 15));
            } else {
                char pctbuf[12];
                fmt_pct(pctbuf, sizeof pctbuf, true, top_pct);
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

    // For OpenCode Go the top bar shows secondary tier (s) instead of primary (p),
    // because OpenCode Go has 3 tiers and the publisher marks secondary as the
    // most useful hero metric on the summary page. The tertiary tier (t) fills
    // the smaller bottom bar.
    provider_kind_t rpk_oc = provider_kind(p->id);
    bool oc_swap = (rpk_oc == PK_OPENCODEGO);
    bool top_has = oc_swap ? p->secondary.has : p->primary.has;
    float top_pct = oc_swap ? p->secondary.pct : p->primary.pct;

    // Percentage label where the provider name used to be
    if (p->ok && top_has) {
        int pv = clampi((int)(top_pct + 0.5f), 0, 100);
        int sv = secondary_pct(p);
        if (sv >= 0) {
            char primary_buf[12];
            char secondary_buf[16];
            snprintf(primary_buf, sizeof primary_buf, "%d%%", pv);
            snprintf(secondary_buf, sizeof secondary_buf, " / %d%%", sv);
            layout_dual_pct_left(row_id[slot], row_val_s[slot],
                primary_buf, secondary_buf,
                (lv_coord_t)(cell->x + 32), (lv_coord_t)(cell->y + 2));
        } else {
            char pctbuf[12];
            fmt_pct(pctbuf, sizeof pctbuf, true, top_pct);
            lv_label_set_text(row_id[slot], pctbuf);
            lv_obj_set_width(row_id[slot], cell->w - 36);
            lv_obj_add_flag(row_val_s[slot], LV_OBJ_FLAG_HIDDEN);
        }
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
                prov_accent(p->id, &tc) ? tc : lv_color_hex(0xe8eaed), 0);
        }
        lv_obj_clear_flag(row_icon[slot], LV_OBJ_FLAG_HIDDEN);
        update_cursor_sess_pulse(row_icon[slot], cursor_sess_refresh_needed(p));
    } else {
        lv_obj_add_flag(row_icon[slot], LV_OBJ_FLAG_HIDDEN);
        update_cursor_sess_pulse(row_icon[slot], false);
    }

    if (!p->ok || !top_has) {
        update_bar_pulse(row_bar[slot], 0.0f, NULL);
        lv_obj_add_flag(row_bar[slot], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(row_id[slot], lv_color_hex(0x6b7075), 0);
    } else {
        int v = clampi((int)(top_pct + 0.5f), 0, 100);
        int fill = bar_fill(v);
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
