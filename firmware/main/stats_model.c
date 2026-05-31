// firmware/main/stats_model.c
#include "stats_model.h"
#include "cJSON.h"
#include "esp_log.h"
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <math.h>

static const char *TAG = "stats";

// Fix K: renamed from cpy() to json_str_copy() for clarity.
static void json_str_copy(char *dst, size_t n, const cJSON *s)
{
    dst[0] = '\0';
    if (cJSON_IsString(s) && s->valuestring) {
        strlcpy(dst, s->valuestring, n);
    }
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

// Fix J: use exact double boundaries matching INT64_MIN/INT64_MAX (2^63).
// The previous approximation (-9.2e18 / 9.2e18) misclassified values in
// the gap between 9.2e18 and the true limit 9223372036854775808.0.
static int64_t i64_clamp(double v)
{
    if (isnan(v)) return 0;
    if (v < -9223372036854775808.0) return INT64_MIN;
    if (v >=  9223372036854775808.0) return INT64_MAX;
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
// 30-day max daily spend/tokens, and a 30-day daily-spend history.
// Reuse the shared cost-shaped fields so the UI can branch on
// provider id without forking the transport/model contract.
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
            if (isnan(d)) continue;
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
    json_str_copy(out->ts, sizeof out->ts, ts);

    if (cJSON_IsArray(ps)) {
        const cJSON *e;
        cJSON_ArrayForEach(e, ps) {
            if (out->n >= STATS_MAX_PROVIDERS) break;
            stats_provider_t *p = &out->p[out->n];
            json_str_copy(p->id, sizeof p->id, cJSON_GetObjectItemCaseSensitive(e, "id"));
            const cJSON *ok = cJSON_GetObjectItemCaseSensitive(e, "ok");
            p->ok = cJSON_IsTrue(ok);
            // p/pr/s/sr are absent on !ok entries — handle gracefully.
            const cJSON *pp = cJSON_GetObjectItemCaseSensitive(e, "p");
            if (cJSON_IsNumber(pp)) { p->has_p = true; p->p = f_sanitize(pp->valuedouble); }
            json_str_copy(p->pr, sizeof p->pr, cJSON_GetObjectItemCaseSensitive(e, "pr"));
            const cJSON *sp = cJSON_GetObjectItemCaseSensitive(e, "s");
            if (cJSON_IsNumber(sp)) { p->has_s = true; p->s = f_sanitize(sp->valuedouble); }
            json_str_copy(p->sr, sizeof p->sr, cJSON_GetObjectItemCaseSensitive(e, "sr"));
            const cJSON *tp = cJSON_GetObjectItemCaseSensitive(e, "t");
            if (cJSON_IsNumber(tp)) { p->has_t = true; p->t = f_sanitize(tp->valuedouble); }
            json_str_copy(p->tr, sizeof p->tr, cJSON_GetObjectItemCaseSensitive(e, "tr"));

            parse_cost(e, p);
            parse_pi(e, p);
            parse_lm(e, p);
            parse_cu(e, p);
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
// Reminder: hidden providers (ollama, opencode, opencodego) are filtered
// in ui.c by is_hidden_provider() — they affect neither order nor slots.
static const char *s_display_order[] = {
    "pi",
    "lmstudio",
    "openrouter",
    "claude",
    "codex",
    "cursor",
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
