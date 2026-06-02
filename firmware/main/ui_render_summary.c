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

// Summary-row secondary bar (row_bar_w): Claude/Codex weekly %, LM Studio
// requests %, or OpenRouter budget %; hidden otherwise. Extracted from render()
// (Fowler audit).
static void render_summary_secondary_bar(int slot, const stats_provider_t *p)
{
    provider_kind_t rpk = provider_kind(p->id);
    if (((rpk == PK_CLAUDE || rpk == PK_CODEX) && p->secondary.has)
        || rpk == PK_LMSTUDIO
        || (rpk == PK_OPENCODEGO && p->secondary.has)) {
        int wv = clampi((int)(p->secondary.pct + 0.5f), 0, 100);
        lv_bar_set_value(row_bar_w[slot], bar_fill(wv), LV_ANIM_OFF);
        lv_obj_set_style_bg_color(row_bar_w[slot], bar_color(p, p->secondary.pct), LV_PART_INDICATOR);
        update_bar_pulse(row_bar_w[slot], p->secondary.pct);
        lv_obj_clear_flag(row_bar_w[slot], LV_OBJ_FLAG_HIDDEN);
    } else if (rpk == PK_OPENROUTER && p->has_cost && p->extra_limit_c > 0) {
        int xv = extra_pct(p);
        lv_bar_set_value(row_bar_w[slot], bar_fill(xv), LV_ANIM_OFF);
        lv_obj_set_style_bg_color(row_bar_w[slot], bar_color(p, (float)xv), LV_PART_INDICATOR);
        update_bar_pulse(row_bar_w[slot], (float)xv);
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

    if (!p->ok || !p->primary.has) {
        update_bar_pulse(row_bar[slot], 0.0f);
        lv_obj_add_flag(row_bar[slot], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(row_val[slot], lv_color_hex(0x6b7075), 0);
        lv_label_set_text(row_val[slot], "off");
    } else {
        int v = clampi((int)(p->primary.pct + 0.5f), 0, 100);
        int fill = bar_fill(v);
        lv_obj_clear_flag(row_bar[slot], LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(row_bar[slot], fill, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(row_bar[slot], bar_color(p, p->primary.pct), LV_PART_INDICATOR);
        update_bar_pulse(row_bar[slot], p->primary.pct);
        lv_obj_set_style_text_color(row_val[slot], lv_color_hex(0xffffff), 0);
        {
            char pctbuf[12];
            fmt_pct(pctbuf, sizeof pctbuf, true, p->primary.pct);
            lv_label_set_text(row_val[slot], pctbuf);
        }
        render_summary_secondary_bar(slot, p);
    }
}
