// firmware/main/ui_format.c
//
// Pure formatting / number / string / provider-resolution helpers split out of
// ui.c. Nothing here touches the shared `st` state or the LVGL widget globals —
// every function is a pure transform over its arguments, so it is safe to call
// from any task (some ARE, e.g. the geometry helpers run off ui_task under the
// same s_mtx discipline as the nav state).
#include "ui.h"
#include "ui_internal.h"
#include "provider_colors.h"
#include "lvgl.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdint.h>

// ── page-grid geometry ───────────────────────────────────────────────────────
ui_page_grid_t ui_grid_from_height(int screen_w, int screen_h)
{
    const int content_h = screen_h - UI_CHROME_TOP - UI_CHROME_BOTTOM;
    return (ui_page_grid_t){
        .content = { 0, UI_CHROME_TOP, screen_w, content_h },
        .cell_w = screen_w / UI_GRID_COLS,
        .cell_h = content_h / UI_GRID_ROWS,
    };
}

ui_rect_t ui_grid_span(const ui_page_grid_t *g, int col, int row, int cols, int rows)
{
    return (ui_rect_t){
        .x = g->content.x + col * g->cell_w,
        .y = g->content.y + row * g->cell_h,
        .w = cols * g->cell_w,
        .h = rows * g->cell_h,
    };
}

int summary_vis_rows_from_grid(const ui_page_grid_t *g)
{
    int rows = g->content.h / g->cell_h;
    if (rows < 1) rows = 1;
    if (rows > ROWS) rows = ROWS;
    rows -= UI_SUMMARY_TOP_ROWS;
    if (rows < 0) rows = 0;
    return rows;
}

// ── provider metric signature (screensaver activity hashing) ─────────────────
bool provider_has_limits_card(const stats_provider_t *p)
{
    return p->has_p || p->has_s || p->has_t || p->pct_hist_n > 0 ||
           (p->has_cost && p->extra_limit_c > 0);
}

int pct_tenths(bool has, float v)
{
    if (!has) return -1;
    int t = (int)(v * 10.0f + 0.5f);
    if (t < 0) t = 0; else if (t > 1000) t = 1000;
    return t;
}

uint32_t hash_mix_u32(uint32_t h, uint32_t v)
{
    h ^= v + 0x9e3779b9U + (h << 6) + (h >> 2);
    return h;
}

uint32_t provider_metric_sig(const stats_provider_t *p)
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

bool provider_card_available(const stats_provider_t *p, card_kind_t card)
{
    switch (card) {
        case CARD_COST:         return p->has_cost || p->has_lm || p->has_cu;
        case CARD_LIMITS:       return provider_has_limits_card(p) || p->has_lm;
    }
    return false;
}

// ── colours / provider resolution ────────────────────────────────────────────
lv_color_t pct_color(float p)   // green -> amber -> red
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
bool prov_accent(const char *id, lv_color_t *out)
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
bool is_hidden_provider(const char *id)
{
    const char *h[] = { HIDDEN_PROVIDERS };
    for (size_t i = 0; i < sizeof(h) / sizeof(h[0]); i++)
        if (strcmp(id, h[i]) == 0) return true;
    return false;
}

// Provider identity enum — resolved once per render path to replace repeated
// strcmp ladders. Resolver for the provider_kind_t enum declared in ui_internal.h.
provider_kind_t provider_kind(const char *id)
{
    if (!id || !id[0]) return PK_UNKNOWN;
    if (strcmp(id, "pi")         == 0) return PK_PI;
    if (strcmp(id, "claude")     == 0) return PK_CLAUDE;
    if (strcmp(id, "codex")      == 0) return PK_CODEX;
    if (strcmp(id, "lmstudio")   == 0) return PK_LMSTUDIO;
    if (strcmp(id, "cursor")     == 0) return PK_CURSOR;
    if (strcmp(id, "openrouter") == 0) return PK_OPENROUTER;
    return PK_UNKNOWN;
}

// Progress-bar indicator color: the provider's theme accent if it has one,
// else the green/amber/red usage ramp.
lv_color_t bar_color(const stats_provider_t *p, float v)
{
    lv_color_t c;
    if (prov_accent(p->id, &c)) return c;
    return pct_color(v);
}

// Bar fill direction. true => 0% draws FULL, 100% draws EMPTY (bars read as
// "headroom remaining"). Color is keyed on the true usage % elsewhere, so this
// affects fill only — never the color.
static const bool s_bar_invert = UI_BAR_INVERT_DEFAULT;

// Map a real 0..100 usage % to the bar's fill value, honoring the flag.
// Only call with an actual percentage — "no data" stays a literal 0 (empty).
int bar_fill(int pct)
{
    if (pct < 0) pct = 0; else if (pct > 100) pct = 100;
    return s_bar_invert ? 100 - pct : pct;
}

// ── token / money / pct / id formatting ──────────────────────────────────────
// tokens -> "123.2M" / "45.6K" / "789" using ONLY integer math (LVGL/newlib
// nano printf has no %lld and no float; everything here fits int32 after the
// divide: 30-day counts are < ~2e9 so /1000 or /1e6 is safe).
// Today's token count for one provider (field varies by provider shape).
int64_t provider_tok_today(const stats_provider_t *p)
{
    switch (provider_kind(p->id)) {
    case PK_LMSTUDIO: return p->has_lm ? p->lm_tok_today : 0;
    case PK_CURSOR:   return p->has_cu ? p->cu_tok_today : 0;
    default:          return p->has_cost ? p->tok_today : 0;
    }
}

const char *summary_provider_name(const char *id)
{
    switch (provider_kind(id)) {
    case PK_PI:         return "Pi";
    case PK_LMSTUDIO:   return "LM Studio";
    case PK_OPENROUTER: return "OpenRouter";
    case PK_CLAUDE:     return "Claude";
    case PK_CODEX:      return "Codex";
    case PK_CURSOR:     return "Cursor";
    default:            return id ? id : "";
    }
}

void fmt_tokens(char *buf, size_t n, int64_t t)
{
    if (t < 0) { snprintf(buf, n, "?"); return; }
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

// tokens -> "76,234,567" (full value, comma-separated; integer math only).
void fmt_tokens_full(char *buf, size_t n, int64_t t)
{
    if (n == 0) return;
    if (t < 0) t = 0;
    if (t == 0) {
        snprintf(buf, n, "0");
        return;
    }
    char tmp[32];
    int pos = (int)sizeof tmp - 1;
    tmp[pos] = '\0';
    int group = 0;
    while (t > 0 && pos > 0) {
        if (group == 3) {
            tmp[--pos] = ',';
            group = 0;
        }
        tmp[--pos] = (char)('0' + (t % 10));
        t /= 10;
        group++;
    }
    size_t len = sizeof tmp - 1 - (size_t)pos;
    if (len >= n) len = n - 1;
    memcpy(buf, tmp + pos, len);
    buf[len] = '\0';
}

// cents -> "$12.47" (integer only; no %f).
// Negative inputs render as "$?" sentinel (corrupt data, not clamped to $0).
void fmt_money(char *buf, size_t n, int32_t cents)
{
    if (cents < 0) { snprintf(buf, n, "$?"); return; }
    // int32_t is `long` on Xtensa -> cast for %d (newlib -Werror=format).
    snprintf(buf, n, "$%d.%02d", (int)(cents / 100), (int)(cents % 100));
}

void fmt_pct(char *buf, size_t n, bool has, float v)
{
    if (!has) { snprintf(buf, n, "--"); return; }
    // Always 1 decimal place. LVGL sprintf has no float support (CONFIG_LV_USE_FLOAT
    // unset), so use integer tenths: 45.3 -> tenths=453 -> "45.3%".
    int tenths = (int)(v * 10.0f + 0.5f);
    if (tenths < 0) tenths = 0; else if (tenths > 1000) tenths = 1000;
    snprintf(buf, n, "%d.%d%%", tenths / 10, tenths % 10);
}

void up_id(char *dst, size_t n, const char *src)
{
    size_t j = 0;
    for (; src && src[j] && j + 1 < n; j++)
        dst[j] = (char)toupper((unsigned char)src[j]);
    dst[j] = '\0';
}

// Extra-usage overage as a clamped 0..100 %. 0 when unknown / no limit.
// Single-sourced: used by both the Cost and Usage-Limits cards.
int extra_pct(const stats_provider_t *p)
{
    if (!p->has_cost || p->extra_limit_c <= 0) return 0;
    int xp = (int)(((int64_t)p->extra_used_c * 100) / p->extra_limit_c);
    if (xp < 0) xp = 0; else if (xp > 100) xp = 100;
    return xp;
}

// ── i64 → i32 history conversion ─────────────────────────────────────────────
// Converts int64_t token-history arrays to int32_t for the bar-chart renderer.
// Values exceeding INT32_MAX are clamped. Used by both lmstudio and cursor paths.
void i64_hist_to_i32(int32_t *dst, const int64_t *src, int n)
{
    for (int i = 0; i < n; i++)
        dst[i] = (int32_t)(src[i] > INT32_MAX ? INT32_MAX : src[i]);
}
