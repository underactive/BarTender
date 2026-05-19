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

            // v2 optional `cost` object (Claude only this build). Absent on v1
            // and on non-Claude providers -> has_cost stays false (memset'd).
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
                if (cJSON_IsNumber(x)) p->extra_used_c  = i32_clamp(x->valuedouble);
                x = cJSON_GetObjectItemCaseSensitive(c, "xl");
                if (cJSON_IsNumber(x)) p->extra_limit_c = i32_clamp(x->valuedouble);
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
            // v2 optional `ph`: 24h session usage-% history (provider-level,
            // sibling of `cost`). Absent => pct_hist_n stays 0 (memset).
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
