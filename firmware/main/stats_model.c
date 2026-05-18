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
    // Audit Contract§MED: forward-guard. This firmware only understands v==1;
    // a future schema bump must not be rendered as best-effort garbage.
    if (out->v != 1) { cJSON_Delete(in); return STATS_PARSE_BAD; }
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
            if (p->id[0]) out->n++;
        }
    }
    cJSON_Delete(in);
    ESP_LOGI(TAG, "parsed v=%d ts=%s n=%d", out->v, out->ts, out->n);
    return STATS_PARSE_OK;
}
