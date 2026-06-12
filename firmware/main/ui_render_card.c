// firmware/main/ui_render_card.c
//
// Card-page rendering functions split out of ui_render.c. All card-specific
// render entry points (render_cost_card, render_limits_card) and their helpers.
// Called from render_card() which is dispatched by render() in ui_render_core.c.
// EVERYTHING here runs on ui_task only.
#include "ui.h"
#include "ui_internal.h"
#include "provider_icons.h"
#include "provider_colors.h"
#include "lvgl.h"
#include <string.h>
#include <stdio.h>

// ── render_cost_card helpers ──────────────────────────────────────────────────

// LM Studio TODAY card: tokens hero + requests line + 30-day bar chart.
// Shared TODAY card body for providers that publish only a local token rollup
// (LM Studio, Cursor): token hero + 30-day bar chart. `show_requests` adds the
// requests sub-line + "Reqs" footer (LM Studio); req_today/req_max are unused
// when false (Cursor). Extracted from render_lmstudio_chart/render_cursor_chart
// (Fowler audit).
static void render_token_chart_card(const stats_provider_t *p,
                                    const ui_page_grid_t *g,
                                    const ui_rect_t *hero,
                                    int64_t today_tok, int64_t max_tok,
                                    const int64_t *hist, int hist_n,
                                    bool show_requests, int req_today, int req_max,
                                    bool card_entered)
{
    cost_tok_set_parent(cost.card);
    lv_obj_t *hide[] = { cost.or_lbl, cost.or_row1, cost.or_row2,
                         cost.bar, cost.bar_lbl, cost.cap };
    for (unsigned i = 0; i < sizeof hide / sizeof *hide; i++)
        lv_obj_add_flag(hide[i], LV_OBJ_FLAG_HIDDEN);
    place_hero_amount(&cost_hero, hero, "TOKENS");
    lv_obj_clear_flag(cost.cost_30, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(cost.chart, LV_OBJ_FLAG_HIDDEN);
    // Chart: grid rows 4..8 (1-indexed); 30-day max on row 9 (below 8-row grid).
    const ui_rect_t chart_r = ui_grid_span(g, 0, 3, 2, 5);
    const ui_rect_t footer_r = ui_grid_span(g, 0, 8, 2, 1);
    lv_obj_set_size(cost.chart, chart_r.w - 24, chart_r.h - 8);
    lv_obj_set_pos(cost.chart, chart_r.x + 12, chart_r.y + 4);
    lv_obj_set_pos(cost.cost_30, footer_r.x + 12, footer_r.y + 2);
    char tk[16], tk30[16];
    fmt_tokens(tk, sizeof tk, today_tok);
    set_hero_amount(&cost_hero, NULL, tk, NULL);   // plain token count, no affix
    fmt_tokens(tk30, sizeof tk30, max_tok);
    if (show_requests) {
        char rq[16], rq30[16];
        snprintf(rq, sizeof rq, "%d", req_today);
        snprintf(rq30, sizeof rq30, "%d", req_max);
        lv_obj_clear_flag(cost.tok, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(cost.tok_unit, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(cost.tok, rq);
        lv_label_set_text(cost.tok_unit, "requests");
        lv_obj_align_to(cost.tok_unit, cost.tok, LV_ALIGN_OUT_RIGHT_BOTTOM, 5, 0);
        lv_label_set_text_fmt(cost.cost_30, "30 DAY MAX: %s Toks " LV_SYMBOL_BULLET "  %s Reqs", tk30, rq30);
    } else {
        lv_obj_add_flag(cost.tok, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(cost.tok_unit, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(cost.cost_30, "30 DAY MAX: %s Toks", tk30);
    }
    int n = hist_n;
    if (n > NAV_HIST_PTS) n = NAV_HIST_PTS;
    lv_color_t cc;
    int32_t ht32[STATS_HIST_MAX];
    i64_hist_to_i32(ht32, hist, n);
    (void)render_cost_bar_chart(cost.chart, cost.ser, ht32, n,
        prov_accent(p->id, &cc) ? cc : lv_color_hex(UI_DEFAULT_CHART_COLOR));
    if (card_entered) anim_chart_fadein(cost.chart);
}

static void render_lmstudio_chart(const stats_provider_t *p,
                                  const ui_page_grid_t *g,
                                  const ui_rect_t *hero,
                                  bool card_entered)
{
    render_token_chart_card(p, g, hero, p->lm_tok_today, p->lm_tok_month_max,
                            p->lm_ht, p->lm_ht_n, true,
                            (int)p->lm_req_today, (int)p->lm_req_month_max,
                            card_entered);
}

// Ollama TODAY card: tokens hero + requests line + 30-day bar chart (same layout as LM Studio).
static void render_ollama_chart(const stats_provider_t *p,
                                const ui_page_grid_t *g,
                                const ui_rect_t *hero,
                                bool card_entered)
{
    render_token_chart_card(p, g, hero, p->ol_tok_today, p->ol_tok_month_max,
                            p->ol_ht, p->ol_ht_n, true,
                            (int)p->ol_req_today, (int)p->ol_req_month_max,
                            card_entered);
}

// Cursor TODAY card: token hero + 30-day bar chart (no requests, no $, no OR rows).
static void render_cursor_chart(const stats_provider_t *p,
                                const ui_page_grid_t *g,
                                const ui_rect_t *hero,
                                bool card_entered)
{
    render_token_chart_card(p, g, hero, p->cu_tok_today, p->cu_tok_month_max,
                            p->cu_ht, p->cu_ht_n, false, 0, 0, card_entered);
}

// Format millicents (1/1000 cent = 1/100000 dollar) for the OpenCode Go cost
// display. The API returns cost in millicents, so the display must use a
// finer-grained format than fmt_money (which expects cents).
//   345     → "$0.00345"   (5 decimal places for sub-cent)
//   12345   → "$0.12345"
//   123456  → "$1.23"      (2 decimal places for dollar+)
static void fmt_milli(char *buf, size_t n, int32_t mc)
{
    if (mc < 0 || n < 3) { snprintf(buf, n, "$?"); return; }
    int dollars = mc / 100000;
    int frac = mc % 100000;
    if (dollars > 0)
        snprintf(buf, n, "$%d.%02d", dollars, frac / 1000);
    else
        snprintf(buf, n, "$0.%05d", frac);
}

// OpenCode Go TODAY card: tokens hero + cost line + 30-day token bar chart.
static void render_opencodego_today(const stats_provider_t *p,
                                    const ui_page_grid_t *g,
                                    const ui_rect_t *hero,
                                    bool card_entered)
{
    lv_obj_t *hide[] = { cost.or_lbl, cost.or_row1, cost.or_row2,
                         cost.bar, cost.bar_lbl };
    for (unsigned i = 0; i < sizeof hide / sizeof *hide; i++)
        lv_obj_add_flag(hide[i], LV_OBJ_FLAG_HIDDEN);
    cost_tok_set_parent(cost.card);
    lv_obj_clear_flag(cost.tok, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(cost.tok_unit, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(cost.cost_30, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(cost.chart, LV_OBJ_FLAG_HIDDEN);

    // Hero: rows 0-1, 2 cols — tokens count with "TOKENS" caption
    place_hero_amount(&cost_hero, hero, "TOKENS");
    char tk[16];
    fmt_tokens(tk, sizeof tk, p->oc_tok_today);
    set_hero_amount(&cost_hero, NULL, tk, NULL);

    // Row 2 — cost line from millicents
    const ui_rect_t cost_r = ui_grid_span(g, 0, 2, 2, 1);
    lv_obj_set_pos(cost.tok, cost_r.x + 12, cost_r.y + 2);
    char ct[16];
    fmt_milli(ct, sizeof ct, p->oc_cost_today_c);
    lv_label_set_text(cost.tok, ct);
    lv_label_set_text(cost.tok_unit, "spend");
    lv_obj_align_to(cost.tok_unit, cost.tok, LV_ALIGN_OUT_RIGHT_BOTTOM, 5, 0);

    // Chart: rows 3-8 — 30-day token bar chart
    const ui_rect_t chart_r = ui_grid_span(g, 0, 3, 2, 5);
    lv_obj_set_size(cost.chart, chart_r.w - 24, chart_r.h - 8);
    lv_obj_set_pos(cost.chart, chart_r.x + 12, chart_r.y + 4);

    // Footer: row 8 — max tokens . max spend
    const ui_rect_t footer_r = ui_grid_span(g, 0, 8, 2, 1);
    lv_obj_set_pos(cost.cost_30, footer_r.x + 12, footer_r.y + 2);
    char tk30[16], ct_today[16];
    fmt_tokens(tk30, sizeof tk30, p->oc_tok_month_max);
    fmt_milli(ct_today, sizeof ct_today, p->oc_cost_today_c);
    lv_label_set_text_fmt(cost.cost_30, "30 DAY MAX: %s Toks  " LV_SYMBOL_BULLET "  %s", tk30, ct_today);

    // Row 8b — resets time from primary usage tier
    lv_obj_clear_flag(cost.cap, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(cost.cap, footer_r.x + 12, footer_r.y + 26);
    if (p->primary.has && p->primary.reset[0]) {
        lv_label_set_text_fmt(cost.cap, "Resets %s", p->primary.reset);
    } else {
        lv_obj_add_flag(cost.cap, LV_OBJ_FLAG_HIDDEN);
    }

    // Render bar chart with token history
    int n = p->oc_ht_n;
    if (n > NAV_HIST_PTS) n = NAV_HIST_PTS;
    lv_color_t cc;
    int32_t ht32[STATS_HIST_MAX];
    i64_hist_to_i32(ht32, p->oc_ht, n);
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

    if (!p->has_cost && !p->has_lm && !p->has_ol && !p->has_cu && !p->has_oc) {
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
    case PK_OLLAMA:
        if (p->has_ol) {
            render_ollama_chart(p, g, hero, card_entered);
            return;
        }
        break;
    case PK_CURSOR:
        if (p->has_cu) {
            render_cursor_chart(p, g, hero, card_entered);
            return;
        }
        break;
    case PK_OPENCODEGO:
        if (p->has_oc) {
            render_opencodego_today(p, g, hero, card_entered);
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

// Ollama STATS card: tokens% hero + requests% line + bar (same layout as LM Studio Stats).
static void render_ollama_stats(const stats_provider_t *p,
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
    if (card_entered && p->has_ol) {
        anim_count_up(lim_hero.num, (int32_t)(p->primary.pct * 10.0f + 0.5f), count_pct_cb);
    } else {
        set_hero_pct(&lim_hero, p->has_ol, p->primary.pct);
    }
    set_bar(lim.s_bar, p->has_ol, p->primary.pct, p);
    lv_obj_add_flag(lim.s_rst, LV_OBJ_FLAG_HIDDEN);
    cost_tok_set_parent(lim.card);
    {
        const ui_rect_t req_r = ui_grid_span(g, 0, 7, 2, 1);
        lv_obj_set_pos(cost.tok, req_r.x + 12, req_r.y + 2);
        lv_obj_set_pos(lim.w_bar, req_r.x + 12, req_r.y + req_r.h - 5);
    }
    if (p->has_ol) {
        char pb[12];
        fmt_pct(pb, sizeof pb, p->has_ol, p->secondary.pct);
        lv_label_set_text(cost.tok, pb);
        lv_label_set_text(cost.tok_unit, "requests");
        lv_obj_align_to(cost.tok_unit, cost.tok, LV_ALIGN_OUT_RIGHT_BOTTOM, 5, 0);
        lv_obj_clear_flag(cost.tok, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(cost.tok_unit, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lim.w_bar, LV_OBJ_FLAG_HIDDEN);
        set_bar(lim.w_bar, true, p->secondary.pct, p);
    } else {
        lv_obj_add_flag(cost.tok, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(cost.tok_unit, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lim.w_bar, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(lim.w_rst, LV_OBJ_FLAG_HIDDEN);
}

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
        anim_count_up(lim_hero.num, (int32_t)(p->primary.pct * 10.0f + 0.5f), count_pct_cb);
    } else {
        set_hero_pct(&lim_hero, p->has_lm, p->primary.pct);
    }
    char pb[12];
    set_bar(lim.s_bar, p->has_lm, p->primary.pct, p);
    lv_obj_add_flag(lim.s_rst, LV_OBJ_FLAG_HIDDEN);
    cost_tok_set_parent(lim.card);
    {
        const ui_rect_t req_r = ui_grid_span(g, 0, 7, 2, 1);
        lv_obj_set_pos(cost.tok, req_r.x + 12, req_r.y + 2);
        lv_obj_set_pos(lim.w_bar, req_r.x + 12, req_r.y + req_r.h - 5);
    }
    if (p->has_lm) {
        fmt_pct(pb, sizeof pb, p->has_lm, p->secondary.pct);
        lv_label_set_text(cost.tok, pb);
        lv_label_set_text(cost.tok_unit, "requests");
        lv_obj_align_to(cost.tok_unit, cost.tok, LV_ALIGN_OUT_RIGHT_BOTTOM, 5, 0);
        lv_obj_clear_flag(cost.tok, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(cost.tok_unit, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lim.w_bar, LV_OBJ_FLAG_HIDDEN);
        set_bar(lim.w_bar, true, p->secondary.pct, p);
    } else {
        lv_obj_add_flag(cost.tok, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(cost.tok_unit, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lim.w_bar, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(lim.w_rst, LV_OBJ_FLAG_HIDDEN);
}

// Show/hide a Limits "tier row" (label + big value + bar, with an optional
// reset label) in one call, replacing the repeated 3-line clear / 4-line
// add-flag blocks. `rst` may be NULL for the extra-usage row (no reset label).
// Extracted from render_limits_card (Fowler audit).
static void show_tier_row(lv_obj_t *lbl, lv_obj_t *big, lv_obj_t *bar)
{
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(big, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_HIDDEN);
}

static void hide_tier_row(lv_obj_t *lbl, lv_obj_t *big, lv_obj_t *bar, lv_obj_t *rst)
{
    lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(big, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
    if (rst) lv_obj_add_flag(rst, LV_OBJ_FLAG_HIDDEN);
}

// Limits secondary tier (lim.a_*): occupies the chart area when
// there's no sparkline and a secondary % exists. OpenRouter (has_balance) has no
// secondary tier. Extracted from render_limits_card (Fowler audit).
static void render_limits_auto(const stats_provider_t *p, bool has_balance,
                                provider_kind_t pk)
{
    if (!has_balance && p->pct_hist_n == 0 && p->secondary.has) {
        char pb[12];
        show_tier_row(lim.a_lbl, lim.a_big, lim.a_bar);
        lv_label_set_text(lim.a_lbl, pk == PK_OPENCODEGO ? "WEEKLY" :
                          (p->tertiary.has ? (pk == PK_CURSOR ? "AUTO / COMPOSER" : "AUTO")
                                           : "WEEKLY"));
        fmt_pct(pb, sizeof pb, p->secondary.has, p->secondary.pct);
        lv_label_set_text(lim.a_big, pb);
        set_bar(lim.a_bar, p->secondary.has, p->secondary.pct, p);
        set_reset_lbl(lim.a_rst, p->secondary.reset);
    } else {
        hide_tier_row(lim.a_lbl, lim.a_big, lim.a_bar, lim.a_rst);
    }
}

// Limits tertiary tier (lim.w_*): "MONTHLY" for OpenCodeGo (otherwise "API")
// when a tertiary % and no sparkline exist, else "WEEKLY" when a sparkline is
// present. Extracted from render_limits_card (Fowler audit).
static void render_limits_weekly(const stats_provider_t *p,
                                  provider_kind_t pk)
{
    char pb[12];
    if (p->tertiary.has && p->pct_hist_n == 0) {
        show_tier_row(lim.w_lbl, lim.w_big, lim.w_bar);
        lv_label_set_text(lim.w_lbl, pk == PK_OPENCODEGO ? "MONTHLY" : "API");
        fmt_pct(pb, sizeof pb, p->tertiary.has, p->tertiary.pct);
        lv_label_set_text(lim.w_big, pb);
        set_bar(lim.w_bar, p->tertiary.has, p->tertiary.pct, p);
        set_reset_lbl(lim.w_rst, p->tertiary.reset);
    } else if (p->secondary.has && p->pct_hist_n > 0) {
        show_tier_row(lim.w_lbl, lim.w_big, lim.w_bar);
        lv_label_set_text(lim.w_lbl, "WEEKLY");
        fmt_pct(pb, sizeof pb, p->secondary.has, p->secondary.pct);
        lv_label_set_text(lim.w_big, pb);
        set_bar(lim.w_bar, p->secondary.has, p->secondary.pct, p);
        set_reset_lbl(lim.w_rst, p->secondary.reset);
    } else {
        hide_tier_row(lim.w_lbl, lim.w_big, lim.w_bar, lim.w_rst);
    }
}

// Limits "EXTRA USAGE" tier (lim.x_*) — or, for OpenRouter (has_balance), the
// remaining-budget line that repurposes cost.tok + lim.w_bar. MUST run after
// render_limits_weekly() since the has_balance branch overrides lim.w_bar.
// Extracted from render_limits_card (Fowler audit).
static void render_limits_extra(const stats_provider_t *p,
                                const ui_page_grid_t *g, bool has_balance)
{
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
            set_bar(lim.w_bar, true, (float)(100 - xp), p);
            update_bar_pulse(lim.x_bar, 0.0f, NULL);
            hide_tier_row(lim.x_lbl, lim.x_val, lim.x_bar, NULL);
        } else {
            show_tier_row(lim.x_lbl, lim.x_val, lim.x_bar);
            char a[16], b[16];
            fmt_money(a, sizeof a, p->extra_used_c);
            fmt_money(b, sizeof b, p->extra_limit_c);
            lv_label_set_text(lim.x_lbl, "EXTRA USAGE");
            lv_label_set_text_fmt(lim.x_val, "%s / %s", a, b);
            lv_bar_set_value(lim.x_bar, bar_fill(xp), LV_ANIM_ON);
            if (!bar_should_pulse((float)xp) || !bar_pulse_uses_color_cycle(p->id)) {
                lv_obj_set_style_bg_color(lim.x_bar, bar_color(p, (float)xp),
                                          LV_PART_INDICATOR);
            }
            update_bar_pulse(lim.x_bar, (float)xp, p->id);
        }
    } else {
        update_bar_pulse(lim.x_bar, 0.0f, NULL);
        hide_tier_row(lim.x_lbl, lim.x_val, lim.x_bar, NULL);
    }
}

// 24h SESSION usage-% sparkline from `ph` (Claude only; absent elsewhere).
// Extracted from render_limits_card (Fowler audit).
static void render_limits_sparkline(const stats_provider_t *p, bool card_entered)
{
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

    // lim_hero is only used by providers migrated to hero_amount (Pi, LM
    // Studio); hide by default so it can't leak onto the others. Those branches
    // re-show it.
    hide_hero_amount(&lim_hero);

    if (pk == PK_LMSTUDIO) {
        render_lmstudio_stats(p, g, hero, body, footer, card_entered);
        return;
    }
    if (pk == PK_OLLAMA) {
        render_ollama_stats(p, g, hero, body, footer, card_entered);
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
                      has_balance ? "API KEY" : (p->tertiary.has ?
                          (pk == PK_OPENCODEGO ? "5-HOUR" : "TOTAL") : "SESSION"));
    if (card_entered && p->primary.has) {
        anim_count_up(lim_hero.num, (int32_t)(p->primary.pct * 10.0f + 0.5f), count_pct_cb);
    } else {
        set_hero_pct(&lim_hero, p->primary.has, p->primary.pct);
    }
    set_bar(lim.s_bar, p->primary.has, p->primary.pct, p);
    if (has_balance) {
        lv_obj_add_flag(lim.s_rst, LV_OBJ_FLAG_HIDDEN);
    } else {
        set_reset_lbl(lim.s_rst, p->primary.reset);
    }

    render_limits_auto(p, has_balance, pk);
    // weekly BEFORE extra: the OpenRouter budget branch in render_limits_extra
    // repurposes lim.w_bar, so it must override whatever weekly set.
    render_limits_weekly(p, pk);
    render_limits_extra(p, g, has_balance);
    render_limits_sparkline(p, card_entered);
}

// ── Card dispatch entry point (called from render() in ui_render_core.c) ─────

void render_card(void)   // ui_task only — dispatcher for the NAV_PAGE card
{
    bool card_entered = (st.prev_nav_level    != NAV_PAGE        ||
                         st.prev_nav_provider != st.nav_provider ||
                         st.prev_nav_card     != st.nav_card);
    st.prev_nav_level    = NAV_PAGE;
    st.prev_nav_provider = st.nav_provider;
    st.prev_nav_card     = st.nav_card;

    hide_summary_chrome();

    if (st.nav_provider < 0 || st.nav_provider >= st.stats.n ||
        st.nav_provider >= STATS_MAX_PROVIDERS) {
        st.nav_level = NAV_SUMMARY;
        return;
    }

    const stats_provider_t *p = &st.stats.p[st.nav_provider];
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
