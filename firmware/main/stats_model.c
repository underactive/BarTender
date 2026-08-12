// firmware/main/stats_model.c
#include "stats_model.h"
#include "cJSON.h"
#include "esp_log.h"
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <math.h>

static const char *TAG = "stats";

// Safely copy a cJSON string value into dst, NUL-terminating on first call
// and using strlcpy for bounded copy. Returns true if a non-empty string was
// copied, false otherwise.
static bool copy_json_string_safe(char *dst, size_t n, const cJSON *s)
{
    dst[0] = '\0';
    if (cJSON_IsString(s) && s->valuestring && s->valuestring[0]) {
        strlcpy(dst, s->valuestring, n);
        return true;
    }
    return false;
}

// Audit Security§MED: cJSON->valuedouble is untrusted (the store could be
// corrupt). Clamp before narrowing so a huge value can't silently wrap into a
// negative/garbage cost or token count.
static int32_t i32_clamp(double v)
{
    if (isnan(v)) return 0;
    if (v <= (double)INT32_MIN) return INT32_MIN;
    if (v >= (double)INT32_MAX) return INT32_MAX;
    return (int32_t)v;
}

// Exact double boundaries matching INT64_MIN/INT64_MAX (±2^63).
// The previous approximation (-9.2e18 / 9.2e18) misclassified values in
// the gap between 9.2e18 and the true limit 9223372036854775808.0.
#define INT64_MIN_AS_DOUBLE  (-9223372036854775808.0)
#define INT64_MAX_AS_DOUBLE   (9223372036854775808.0)

static int64_t i64_clamp(double v)
{
    if (isnan(v)) return 0;
    if (v < INT64_MIN_AS_DOUBLE) return INT64_MIN;
    if (v >= INT64_MAX_AS_DOUBLE) return INT64_MAX;
    return (int64_t)v;
}

// Audit (Backend§MED): float fields are stored verbatim from the untrusted
// cJSON->valuedouble. Unlike the integer paths (i32/i64_clamp), the (float)
// casts skipped NaN/Inf handling, so a corrupt store could push NaN or Inf
// straight into UI formatting (and NaN compares false against every bound,
// defeating later clamps). Sanitize NaN/Inf -> 0 here; leave the magnitude of
// finite values intact so the parse layer stays data-preserving (usage % can
// legitimately exceed 100 in overage — the UI clamps for rendering, not us).
static float f_sanitize(double v)
{
    if (isnan(v) || isinf(v)) return 0.0f;
    return (float)v;
}

// Fetch + typecheck + clamp a single JSON number field in one call, collapsing
// the repeated `x = Get(o,key); if (IsNumber(x)) dst = clamp(x->valuedouble);`
// pairs. Returns true iff `key` was present as a number, so callers that track
// "did this block carry any real data" can OR the result into their any_* flag.
static bool get_i32(const cJSON *o, const char *key, int32_t *dst)
{
    const cJSON *x = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(x)) return false;
    *dst = i32_clamp(x->valuedouble);
    return true;
}

static bool get_i64(const cJSON *o, const char *key, int64_t *dst)
{
    const cJSON *x = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(x)) return false;
    *dst = i64_clamp(x->valuedouble);
    return true;
}

static bool get_f(const cJSON *o, const char *key, float *dst)
{
    const cJSON *x = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(x)) return false;
    *dst = f_sanitize(x->valuedouble);
    return true;
}

// ---- per-block static helpers (fix H: extracted from the provider loop) ----

// v2 optional `cost` object. Absent on v1 and on providers with no cost data
// -> has_cost stays false (memset'd).
static void parse_cost(const cJSON *e, stats_provider_t *p)
{
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(e, "cost");
    if (!cJSON_IsObject(c)) return;
    p->has_cost = true;
    get_i32(c, "ct", &p->cost_today_c);
    get_i32(c, "cm", &p->cost_month_c);
    get_i64(c, "tt", &p->tok_today);
    get_i64(c, "tm", &p->tok_month);
    get_i32(c, "xu", &p->extra_used_c);
    get_i32(c, "xl", &p->extra_limit_c);
    get_i32(c, "cw", &p->cost_week_c);
    get_i32(c, "cr", &p->credits_remaining_c);
    get_i32(c, "cl", &p->credits_limit_c);
    const cJSON *h = cJSON_GetObjectItemCaseSensitive(c, "h");
    if (cJSON_IsArray(h)) {
        const cJSON *hv;
        cJSON_ArrayForEach(hv, h) {
            if (p->hist_n >= STATS_HIST_MAX) break;
            if (cJSON_IsNumber(hv))
                p->hist[p->hist_n++] = i32_clamp(hv->valuedouble);
        }
    }
}

// v2 optional `pi` block: Pi Agent publishes today's spend/tokens,
// 30-day max daily spend/tokens, a 30-day daily-spend history (h), and a
// 30-day daily-token history (ht) for the summary avg bar. Reuse the shared
// cost-shaped fields so the UI can branch on provider id without forking the
// transport/model contract.
static void parse_pi(const cJSON *e, stats_provider_t *p)
{
    const cJSON *pi = cJSON_GetObjectItemCaseSensitive(e, "pi");
    if (strcmp(p->id, "pi") != 0 || !cJSON_IsObject(pi)) return;
    bool any_pi = false;
    if (get_i32(pi, "ts", &p->cost_today_c)) any_pi = true;
    if (get_i64(pi, "tt", &p->tok_today))    any_pi = true;
    if (get_i32(pi, "ps", &p->cost_month_c)) any_pi = true;
    if (get_i64(pi, "pt", &p->tok_month))    any_pi = true;
    const cJSON *h = cJSON_GetObjectItemCaseSensitive(pi, "h");
    if (cJSON_IsArray(h)) {
        p->hist_n = 0; // Pi history owns the chart for this provider.
        const cJSON *hv;
        cJSON_ArrayForEach(hv, h) {
            if (p->hist_n >= STATS_HIST_MAX) break;
            if (cJSON_IsNumber(hv)) {
                p->hist[p->hist_n++] = i32_clamp(hv->valuedouble);
                any_pi = true;
            }
        }
    }
    const cJSON *pht = cJSON_GetObjectItemCaseSensitive(pi, "ht");
    if (cJSON_IsArray(pht)) {
        const cJSON *hv;
        cJSON_ArrayForEach(hv, pht) {
            if (p->pi_ht_n >= STATS_HIST_MAX) break;
            if (cJSON_IsNumber(hv)) {
                p->pi_ht[p->pi_ht_n++] = i64_clamp(hv->valuedouble);
                any_pi = true;
            }
        }
    }
    if (any_pi) p->has_cost = true;
}

// v2 optional `lm` block: LM Studio publishes local inference
// metrics — requests, tokens, cache, top models, 7-day table.
// Dedicated fields, not cost-shaped slot reuse.
static void parse_lm(const cJSON *e, stats_provider_t *p)
{
    const cJSON *lm = cJSON_GetObjectItemCaseSensitive(e, "lm");
    if (strcmp(p->id, "lmstudio") != 0 || !cJSON_IsObject(lm)) return;
    bool any_lm = false;
    if (get_i32(lm, "rq",  &p->lm_req_today))     any_lm = true;
    if (get_i64(lm, "tk",  &p->lm_tok_today))     any_lm = true;
    if (get_i32(lm, "mxr", &p->lm_req_month_max)) any_lm = true;
    if (get_i64(lm, "mxt", &p->lm_tok_month_max)) any_lm = true;
    if (get_f(lm, "cp", &p->lm_cache_pct))        any_lm = true;
    if (get_f(lm, "ch", &p->lm_cache_hit_pct))    any_lm = true;
    const cJSON *hr = cJSON_GetObjectItemCaseSensitive(lm, "hr");
    if (cJSON_IsArray(hr)) {
        const cJSON *hv;
        cJSON_ArrayForEach(hv, hr) {
            if (p->lm_hr_n >= STATS_HIST_MAX) break;
            if (cJSON_IsNumber(hv)) { p->lm_hr[p->lm_hr_n++] = i32_clamp(hv->valuedouble); any_lm = true; }
        }
    }
    const cJSON *ht = cJSON_GetObjectItemCaseSensitive(lm, "ht");
    if (cJSON_IsArray(ht)) {
        const cJSON *hv;
        cJSON_ArrayForEach(hv, ht) {
            if (p->lm_ht_n >= STATS_HIST_MAX) break;
            if (cJSON_IsNumber(hv)) { p->lm_ht[p->lm_ht_n++] = i64_clamp(hv->valuedouble); any_lm = true; }
        }
    }
    const cJSON *models = cJSON_GetObjectItemCaseSensitive(lm, "models");
    if (cJSON_IsArray(models)) {
        const cJSON *mv;
        cJSON_ArrayForEach(mv, models) {
            if (p->lm_models_n >= LM_MODELS_MAX) break;
            if (!cJSON_IsObject(mv)) continue;
            const cJSON *mid = cJSON_GetObjectItemCaseSensitive(mv, "id");
            const cJSON *mrq = cJSON_GetObjectItemCaseSensitive(mv, "rq");
            if (cJSON_IsString(mid) && mid->valuestring && cJSON_IsNumber(mrq)) {
                strlcpy(p->lm_models_id[p->lm_models_n], mid->valuestring, STATS_ID_MAX);
                p->lm_models_req[p->lm_models_n] = i32_clamp(mrq->valuedouble);
                p->lm_models_n++;
                any_lm = true;
            }
        }
    }
    const cJSON *week = cJSON_GetObjectItemCaseSensitive(lm, "week");
    if (cJSON_IsArray(week)) {
        const cJSON *wv;
        cJSON_ArrayForEach(wv, week) {
            if (p->lm_week_n >= LM_WEEK_MAX) break;
            if (!cJSON_IsObject(wv)) continue;
            const cJSON *wd = cJSON_GetObjectItemCaseSensitive(wv, "d");
            if (cJSON_IsString(wd) && wd->valuestring) {
                // Fix I: use sizeof(element) instead of the magic literal 6.
                strlcpy(p->lm_week_d[p->lm_week_n], wd->valuestring,
                        sizeof(p->lm_week_d[p->lm_week_n]));
                get_i32(wv, "rq", &p->lm_week_rq[p->lm_week_n]);
                get_i64(wv, "tk", &p->lm_week_tk[p->lm_week_n]);
                get_f(wv,  "cp", &p->lm_week_cp[p->lm_week_n]);
                get_f(wv,  "ch", &p->lm_week_ch[p->lm_week_n]);
                p->lm_week_n++;
                any_lm = true;
            }
        }
    }
    if (any_lm) p->has_lm = true;
}

// v2 optional `ol` block: Ollama publishes local inference
// metrics — requests, tokens, 7-day table. Dedicated fields,
// not cost-slot reuse.
static void parse_ol(const cJSON *e, stats_provider_t *p)
{
    const cJSON *ol = cJSON_GetObjectItemCaseSensitive(e, "ol");
    if (strcmp(p->id, "ollama") != 0 || !cJSON_IsObject(ol)) return;
    bool any_ol = false;
    if (get_i32(ol, "rq",  &p->ol_req_today))     any_ol = true;
    if (get_i64(ol, "tk",  &p->ol_tok_today))     any_ol = true;
    if (get_i32(ol, "mxr", &p->ol_req_month_max)) any_ol = true;
    if (get_i64(ol, "mxt", &p->ol_tok_month_max)) any_ol = true;
    const cJSON *hr = cJSON_GetObjectItemCaseSensitive(ol, "hr");
    if (cJSON_IsArray(hr)) {
        const cJSON *hv;
        cJSON_ArrayForEach(hv, hr) {
            if (p->ol_hr_n >= STATS_HIST_MAX) break;
            if (cJSON_IsNumber(hv)) { p->ol_hr[p->ol_hr_n++] = i32_clamp(hv->valuedouble); any_ol = true; }
        }
    }
    const cJSON *ht = cJSON_GetObjectItemCaseSensitive(ol, "ht");
    if (cJSON_IsArray(ht)) {
        const cJSON *hv;
        cJSON_ArrayForEach(hv, ht) {
            if (p->ol_ht_n >= STATS_HIST_MAX) break;
            if (cJSON_IsNumber(hv)) { p->ol_ht[p->ol_ht_n++] = i64_clamp(hv->valuedouble); any_ol = true; }
        }
    }
    const cJSON *week = cJSON_GetObjectItemCaseSensitive(ol, "week");
    if (cJSON_IsArray(week)) {
        const cJSON *wv;
        cJSON_ArrayForEach(wv, week) {
            if (p->ol_week_n >= OL_WEEK_MAX) break;
            if (!cJSON_IsObject(wv)) continue;
            const cJSON *wd = cJSON_GetObjectItemCaseSensitive(wv, "d");
            if (cJSON_IsString(wd) && wd->valuestring) {
                strlcpy(p->ol_week_d[p->ol_week_n], wd->valuestring,
                        sizeof(p->ol_week_d[p->ol_week_n]));
                get_i32(wv, "rq", &p->ol_week_rq[p->ol_week_n]);
                get_i64(wv, "tk", &p->ol_week_tk[p->ol_week_n]);
                p->ol_week_n++;
                any_ol = true;
            }
        }
    }
    if (any_ol) p->has_ol = true;
}

// v2 optional `cu` block: Cursor publishes Mac-local token rollup
// from cursor-stats.sh (no cost/requests).
static void parse_cu(const cJSON *e, stats_provider_t *p)
{
    const cJSON *cu = cJSON_GetObjectItemCaseSensitive(e, "cu");
    if (strcmp(p->id, "cursor") != 0 || !cJSON_IsObject(cu)) return;
    const cJSON *x;
    bool any_cu = false;
    p->cu_sess_ok = true;
    x = cJSON_GetObjectItemCaseSensitive(cu, "sess");
    if (cJSON_IsBool(x) && !cJSON_IsTrue(x)) p->cu_sess_ok = false;
    if (get_i64(cu, "tk",  &p->cu_tok_today))     any_cu = true;
    if (get_i64(cu, "mxt", &p->cu_tok_month_max)) any_cu = true;
    const cJSON *ht = cJSON_GetObjectItemCaseSensitive(cu, "ht");
    if (cJSON_IsArray(ht)) {
        const cJSON *hv;
        cJSON_ArrayForEach(hv, ht) {
            if (p->cu_ht_n >= STATS_HIST_MAX) break;
            if (cJSON_IsNumber(hv)) {
                p->cu_ht[p->cu_ht_n++] = i64_clamp(hv->valuedouble);
                any_cu = true;
            }
        }
    }
    if (any_cu) p->has_cu = true;
}

// v2 optional `oc` block: OpenCode Go token/cost rollup from opencode.ai API.
// Dedicated fields (not cost-slot reuse) — token-focused hero, cost is secondary text.
static void parse_oc(const cJSON *e, stats_provider_t *p)
{
    const cJSON *oc = cJSON_GetObjectItemCaseSensitive(e, "oc");
    if (strcmp(p->id, "opencodego") != 0 || !cJSON_IsObject(oc)) return;
    bool any_oc = false;
    if (get_i64(oc, "tk",  &p->oc_tok_today))    any_oc = true;
    if (get_i32(oc, "ct",  &p->oc_cost_today_c)) any_oc = true;
    if (get_i64(oc, "mxt", &p->oc_tok_month_max)) any_oc = true;
    const cJSON *ht = cJSON_GetObjectItemCaseSensitive(oc, "ht");
    if (cJSON_IsArray(ht)) {
        const cJSON *hv;
        cJSON_ArrayForEach(hv, ht) {
            if (p->oc_ht_n >= STATS_HIST_MAX) break;
            if (cJSON_IsNumber(hv)) {
                p->oc_ht[p->oc_ht_n++] = i64_clamp(hv->valuedouble);
                any_oc = true;
            }
        }
    }
    if (any_oc) p->has_oc = true;
}

// v2 optional `mo` block: Xiaomi MiMo token/cost rollup from platform.xiaomimimo.com API.
// Dedicated fields (not cost-slot reuse) — token-focused hero, cost is secondary text.
static void parse_mo(const cJSON *e, stats_provider_t *p)
{
    const cJSON *mo = cJSON_GetObjectItemCaseSensitive(e, "mo");
    if (strcmp(p->id, "mimo") != 0 || !cJSON_IsObject(mo)) return;
    bool any_mo = false;
    if (get_i64(mo, "tk",  &p->mo_tok_today))    any_mo = true;
    if (get_i32(mo, "ct",  &p->mo_cost_today_c)) any_mo = true;
    if (get_i64(mo, "mxt", &p->mo_tok_month_max)) any_mo = true;
    if (get_i32(mo, "bl",  &p->mo_balance_c))      any_mo = true;
    if (get_i32(mo, "gbl", &p->mo_gift_balance_c))  any_mo = true;
    const cJSON *ht = cJSON_GetObjectItemCaseSensitive(mo, "ht");
    if (cJSON_IsArray(ht)) {
        const cJSON *hv;
        cJSON_ArrayForEach(hv, ht) {
            if (p->mo_ht_n >= STATS_HIST_MAX) break;
            if (cJSON_IsNumber(hv)) {
                p->mo_ht[p->mo_ht_n++] = i64_clamp(hv->valuedouble);
                any_mo = true;
            }
        }
    }
    if (any_mo) p->has_mo = true;
}

// v2 optional `ph`: 24h usage-% history (provider-level, sibling of `cost`).
// Absent => pct_hist_n stays 0 (memset). For Pi provider: represents Current
// vs Max (today vs peak usage).
static void parse_ph(const cJSON *e, stats_provider_t *p)
{
    const cJSON *ph = cJSON_GetObjectItemCaseSensitive(e, "ph");
    if (!cJSON_IsArray(ph)) return;
    const cJSON *pv;
    cJSON_ArrayForEach(pv, ph) {
        if (p->pct_hist_n >= STATS_PCT_HIST_MAX) break;
        if (cJSON_IsNumber(pv)) {
            // Guard before narrowing: (int)NaN / (int)Inf is undefined behavior,
            // and would slip past the < 0 / > 100 clamp below.
            double d = pv->valuedouble;
            int v = (d <= 0.0) ? 0 : (d >= 100.0) ? 100 : (int)d;
            p->pct_hist[p->pct_hist_n++] = (uint8_t)v;
        }
    }
}

// ---------------------------------------------------------------------------

stats_parse_t stats_model_parse(const char *body, stats_t *out)
{
    if (!body || !out) return STATS_PARSE_BAD;
    memset(out, 0, sizeof *out);

    // --- Step 1: Upstash envelope {"result":"..."} ---
    cJSON *env = cJSON_Parse(body);
    if (!env) return STATS_PARSE_BAD;
    cJSON *res = cJSON_GetObjectItemCaseSensitive(env, "result");
    // Audit Contract§MED: only "absent" or "null" means not-yet-published.
    // A present-but-non-string result is a corrupt store value → BAD, so the
    // UI shows "bad data" instead of a misleading "waiting for publisher".
    if (!res || cJSON_IsNull(res)) {
        cJSON_Delete(env);
        return STATS_PARSE_NO_DATA;
    }
    if (!cJSON_IsString(res) || !res->valuestring || !res->valuestring[0]) {
        cJSON_Delete(env);
        return STATS_PARSE_BAD;
    }

    // --- Step 2: inner payload (result is itself a JSON string) ---
    cJSON *in = cJSON_Parse(res->valuestring);
    cJSON_Delete(env);
    if (!in) return STATS_PARSE_BAD;

    const cJSON *v  = cJSON_GetObjectItemCaseSensitive(in, "v");
    const cJSON *ts = cJSON_GetObjectItemCaseSensitive(in, "ts");
    const cJSON *ps = cJSON_GetObjectItemCaseSensitive(in, "providers");
    // Guard the narrowing: (int)NaN / (int)Inf is undefined behavior. A NaN
    // version would otherwise hit (int)v->valuedouble before the v1/v2 gate.
    out->v = (cJSON_IsNumber(v) && !isnan(v->valuedouble) && !isinf(v->valuedouble))
                 ? (int)v->valuedouble : 0;
    // Audit Contract§MED: forward-guard. This firmware understands v1 and v2
    // (v2 = v1 + optional `cost` block, strict superset). A v3+ schema bump
    // must still be rejected, not rendered as best-effort garbage.
    if (out->v != 1 && out->v != 2) { cJSON_Delete(in); return STATS_PARSE_BAD; }
    copy_json_string_safe(out->ts, sizeof out->ts, ts);

    if (cJSON_IsArray(ps)) {
        const cJSON *e;
        cJSON_ArrayForEach(e, ps) {
            if (out->n >= STATS_MAX_PROVIDERS) break;
            stats_provider_t *p = &out->p[out->n];
            copy_json_string_safe(p->id, sizeof p->id, cJSON_GetObjectItemCaseSensitive(e, "id"));
            const cJSON *ok = cJSON_GetObjectItemCaseSensitive(e, "ok");
            p->ok = cJSON_IsTrue(ok);
            // p/pr/s/sr are absent on !ok entries — handle gracefully.
            const cJSON *pp = cJSON_GetObjectItemCaseSensitive(e, "p");
            if (cJSON_IsNumber(pp)) { p->primary.has = true; p->primary.pct = f_sanitize(pp->valuedouble); }
            copy_json_string_safe(p->primary.reset, sizeof p->primary.reset, cJSON_GetObjectItemCaseSensitive(e, "pr"));
            const cJSON *sp = cJSON_GetObjectItemCaseSensitive(e, "s");
            if (cJSON_IsNumber(sp)) { p->secondary.has = true; p->secondary.pct = f_sanitize(sp->valuedouble); }
            copy_json_string_safe(p->secondary.reset, sizeof p->secondary.reset, cJSON_GetObjectItemCaseSensitive(e, "sr"));
            const cJSON *tp = cJSON_GetObjectItemCaseSensitive(e, "t");
            if (cJSON_IsNumber(tp)) { p->tertiary.has = true; p->tertiary.pct = f_sanitize(tp->valuedouble); }
            copy_json_string_safe(p->tertiary.reset, sizeof p->tertiary.reset, cJSON_GetObjectItemCaseSensitive(e, "tr"));

            // All per-provider parse_* functions below depend on p->id being
            // populated first. Do not reorder above the copy_json_string_safe
            // call for id.
            parse_cost(e, p);
            parse_pi(e, p);
            parse_lm(e, p);
            parse_ol(e, p);
            parse_cu(e, p);
            parse_oc(e, p);
            parse_mo(e, p);
            parse_ph(e, p);

            if (p->id[0]) out->n++;
        }
    }
    cJSON_Delete(in);
    ESP_LOGI(TAG, "parsed v=%d ts=%s n=%d", out->v, out->ts, out->n);
    return STATS_PARSE_OK;
}

// ---- display ordering -----------------------------------------------------

// Canonical summary-page display sequence. Providers not listed here follow
// in their original relative order at the bottom of the list.
// Reminder: hidden providers (ollama, opencode) are filtered
// in ui.c by is_hidden_provider() — they affect neither order nor slots.
// Grid is row-major over 2 columns, so this flat list reads as the on-screen
// layout (left,right per row):
//   Pi          LM Studio
//   Claude      Codex
//   Cursor      OpenCode Go
//   Qwen        OpenRouter
//   MiMo        Moonshot
//   DeepSeek    Ramp
// (codex is the "OpenAI" provider). Anything unlisted follows below.
static const char *s_display_order[] = {
    "pi",
    "lmstudio",
    "claude",
    "codex",
    "cursor",
    "opencodego",
    "qwencloud",
    "openrouter",
    "mimo",
    "moonshot",
    "deepseek",
    "ramp",
    "ollama",
};

void stats_model_reorder(stats_t *stats)
{
    if (!stats || stats->n <= 1) return;

    // Priority per slot: -1 = unknown (lowest), 0..N = display index
    int prio[STATS_MAX_PROVIDERS];
    const unsigned n_order = sizeof(s_display_order) / sizeof(s_display_order[0]);
    for (int i = 0; i < stats->n; i++) {
        prio[i] = -1;
        for (unsigned oi = 0; oi < n_order; oi++) {
            if (strcmp(stats->p[i].id, s_display_order[oi]) == 0) {
                prio[i] = (int)oi;
                break;
            }
        }
    }

    // Stable bubble sort by priority (n ≤ 12, trivial). Unknown providers
    // (prio=-1) sink below all known ones. Ties preserve original order.
    for (int i = 0; i < stats->n - 1; i++) {
        for (int j = 0; j < stats->n - 1 - i; j++) {
            if (prio[j + 1] != -1 && (prio[j] == -1 || prio[j + 1] < prio[j])) {
                stats_provider_t tmp = stats->p[j];
                stats->p[j] = stats->p[j + 1];
                stats->p[j + 1] = tmp;
                int tp = prio[j];
                prio[j] = prio[j + 1];
                prio[j + 1] = tp;
            }
        }
    }
}
