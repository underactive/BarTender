// firmware/main/stats_model.c
#include "stats_model.h"
#include "cJSON.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "stats";

static void cpy(char *dst, size_t n, const cJSON *s)
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
    if (v <= (double)INT32_MIN) return INT32_MIN;
    if (v >= (double)INT32_MAX) return INT32_MAX;
    return (int32_t)v;
}
static int64_t i64_clamp(double v)
{
    if (v <= -9.2e18) return INT64_MIN;
    if (v >=  9.2e18) return INT64_MAX;
    return (int64_t)v;
}

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
    out->v = cJSON_IsNumber(v) ? (int)v->valuedouble : 0;
    // Audit Contract§MED: forward-guard. This firmware understands v1 and v2
    // (v2 = v1 + optional `cost` block, strict superset). A v3+ schema bump
    // must still be rejected, not rendered as best-effort garbage.
    if (out->v != 1 && out->v != 2) { cJSON_Delete(in); return STATS_PARSE_BAD; }
    cpy(out->ts, sizeof out->ts, ts);

    if (cJSON_IsArray(ps)) {
        const cJSON *e;
        cJSON_ArrayForEach(e, ps) {
            if (out->n >= STATS_MAX_PROVIDERS) break;
            stats_provider_t *p = &out->p[out->n];
            cpy(p->id, sizeof p->id, cJSON_GetObjectItemCaseSensitive(e, "id"));
            const cJSON *ok = cJSON_GetObjectItemCaseSensitive(e, "ok");
            p->ok = cJSON_IsTrue(ok);
            // p/pr/s/sr are absent on !ok entries — handle gracefully.
            const cJSON *pp = cJSON_GetObjectItemCaseSensitive(e, "p");
            if (cJSON_IsNumber(pp)) { p->has_p = true; p->p = (float)pp->valuedouble; }
            cpy(p->pr, sizeof p->pr, cJSON_GetObjectItemCaseSensitive(e, "pr"));
            const cJSON *sp = cJSON_GetObjectItemCaseSensitive(e, "s");
            if (cJSON_IsNumber(sp)) { p->has_s = true; p->s = (float)sp->valuedouble; }
            cpy(p->sr, sizeof p->sr, cJSON_GetObjectItemCaseSensitive(e, "sr"));
            const cJSON *tp = cJSON_GetObjectItemCaseSensitive(e, "t");
            if (cJSON_IsNumber(tp)) { p->has_t = true; p->t = (float)tp->valuedouble; }
            cpy(p->tr, sizeof p->tr, cJSON_GetObjectItemCaseSensitive(e, "tr"));

            // v2 optional `cost` object. Absent on v1 and on providers with no
            // cost data -> has_cost stays false (memset'd).
            const cJSON *c = cJSON_GetObjectItemCaseSensitive(e, "cost");
            if (cJSON_IsObject(c)) {
                p->has_cost = true;
                const cJSON *x;
                x = cJSON_GetObjectItemCaseSensitive(c, "ct");
                if (cJSON_IsNumber(x)) p->cost_today_c  = i32_clamp(x->valuedouble);
                x = cJSON_GetObjectItemCaseSensitive(c, "cm");
                if (cJSON_IsNumber(x)) p->cost_month_c  = i32_clamp(x->valuedouble);
                x = cJSON_GetObjectItemCaseSensitive(c, "tt");
                if (cJSON_IsNumber(x)) p->tok_today     = i64_clamp(x->valuedouble);
                x = cJSON_GetObjectItemCaseSensitive(c, "tm");
                if (cJSON_IsNumber(x)) p->tok_month     = i64_clamp(x->valuedouble);
                x = cJSON_GetObjectItemCaseSensitive(c, "xu");
                if (cJSON_IsNumber(x)) p->extra_used_c      = i32_clamp(x->valuedouble);
                x = cJSON_GetObjectItemCaseSensitive(c, "xl");
                if (cJSON_IsNumber(x)) p->extra_limit_c     = i32_clamp(x->valuedouble);
                x = cJSON_GetObjectItemCaseSensitive(c, "cw");
                if (cJSON_IsNumber(x)) p->cost_week_c        = i32_clamp(x->valuedouble);
                x = cJSON_GetObjectItemCaseSensitive(c, "cr");
                if (cJSON_IsNumber(x)) p->credits_remaining_c = i32_clamp(x->valuedouble);
                x = cJSON_GetObjectItemCaseSensitive(c, "cl");
                if (cJSON_IsNumber(x)) p->credits_limit_c    = i32_clamp(x->valuedouble);
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
            const cJSON *pi = cJSON_GetObjectItemCaseSensitive(e, "pi");
            if (strcmp(p->id, "pi") == 0 && cJSON_IsObject(pi)) {
                const cJSON *x;
                bool any_pi = false;
                x = cJSON_GetObjectItemCaseSensitive(pi, "ts");
                if (cJSON_IsNumber(x)) {
                    p->cost_today_c = i32_clamp(x->valuedouble);
                    any_pi = true;
                }
                x = cJSON_GetObjectItemCaseSensitive(pi, "tt");
                if (cJSON_IsNumber(x)) {
                    p->tok_today = i64_clamp(x->valuedouble);
                    any_pi = true;
                }
                x = cJSON_GetObjectItemCaseSensitive(pi, "ps");
                if (cJSON_IsNumber(x)) {
                    p->cost_month_c = i32_clamp(x->valuedouble);
                    any_pi = true;
                }
                x = cJSON_GetObjectItemCaseSensitive(pi, "pt");
                if (cJSON_IsNumber(x)) {
                    p->tok_month = i64_clamp(x->valuedouble);
                    any_pi = true;
                }
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
            const cJSON *lm = cJSON_GetObjectItemCaseSensitive(e, "lm");
            if (strcmp(p->id, "lmstudio") == 0 && cJSON_IsObject(lm)) {
                const cJSON *x;
                bool any_lm = false;
                x = cJSON_GetObjectItemCaseSensitive(lm, "rq");
                if (cJSON_IsNumber(x)) { p->lm_req_today = i32_clamp(x->valuedouble); any_lm = true; }
                x = cJSON_GetObjectItemCaseSensitive(lm, "tk");
                if (cJSON_IsNumber(x)) { p->lm_tok_today = i64_clamp(x->valuedouble); any_lm = true; }
                x = cJSON_GetObjectItemCaseSensitive(lm, "mxr");
                if (cJSON_IsNumber(x)) { p->lm_req_month_max = i32_clamp(x->valuedouble); any_lm = true; }
                x = cJSON_GetObjectItemCaseSensitive(lm, "mxt");
                if (cJSON_IsNumber(x)) { p->lm_tok_month_max = i64_clamp(x->valuedouble); any_lm = true; }
                x = cJSON_GetObjectItemCaseSensitive(lm, "cp");
                if (cJSON_IsNumber(x)) { p->lm_cache_pct = (float)x->valuedouble; any_lm = true; }
                x = cJSON_GetObjectItemCaseSensitive(lm, "ch");
                if (cJSON_IsNumber(x)) { p->lm_cache_hit_pct = (float)x->valuedouble; any_lm = true; }
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
                        const cJSON *wrq = cJSON_GetObjectItemCaseSensitive(wv, "rq");
                        const cJSON *wtk = cJSON_GetObjectItemCaseSensitive(wv, "tk");
                        const cJSON *wcp = cJSON_GetObjectItemCaseSensitive(wv, "cp");
                        const cJSON *wch = cJSON_GetObjectItemCaseSensitive(wv, "ch");
                        if (cJSON_IsString(wd) && wd->valuestring) {
                            strlcpy(p->lm_week_d[p->lm_week_n], wd->valuestring, 6);
                            if (cJSON_IsNumber(wrq)) p->lm_week_rq[p->lm_week_n] = i32_clamp(wrq->valuedouble);
                            if (cJSON_IsNumber(wtk)) p->lm_week_tk[p->lm_week_n] = i64_clamp(wtk->valuedouble);
                            if (cJSON_IsNumber(wcp)) p->lm_week_cp[p->lm_week_n] = (float)wcp->valuedouble;
                            if (cJSON_IsNumber(wch)) p->lm_week_ch[p->lm_week_n] = (float)wch->valuedouble;
                            p->lm_week_n++;
                            any_lm = true;
                        }
                    }
                }
                if (any_lm) p->has_lm = true;
            }
            // v2 optional `ph`: 24h usage-% history (provider-level,
            // sibling of `cost`). Absent => pct_hist_n stays 0 (memset).
            // For Pi provider: represents Current vs Max (today vs peak usage).
            const cJSON *ph = cJSON_GetObjectItemCaseSensitive(e, "ph");
            if (cJSON_IsArray(ph)) {
                const cJSON *pv;
                cJSON_ArrayForEach(pv, ph) {
                    if (p->pct_hist_n >= STATS_PCT_HIST_MAX) break;
                    if (cJSON_IsNumber(pv)) {
                        int v = (int)pv->valuedouble;
                        if (v < 0) v = 0; else if (v > 100) v = 100;
                        p->pct_hist[p->pct_hist_n++] = (uint8_t)v;
                    }
                }
            }
            if (p->id[0]) out->n++;
        }
    }
    cJSON_Delete(in);
    ESP_LOGI(TAG, "parsed v=%d ts=%s n=%d", out->v, out->ts, out->n);
    return STATS_PARSE_OK;
}
