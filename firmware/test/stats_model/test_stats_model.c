// firmware/test/stats_model/test_stats_model.c
//
// Host-compiled table-tests for stats_model_parse() and stats_model_reorder().
// Compiles the REAL firmware/main/stats_model.c against the vendored cJSON and
// a no-op esp_log.h shim. No mocking of internals — only the public API is
// exercised.

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <math.h>

#include "stats_model.h"

// ---------------------------------------------------------------------------
// Tiny assert framework
// ---------------------------------------------------------------------------

static int g_pass = 0;
static int g_fail = 0;
static const char *g_current_test = "(no test)";

#define TEST(name) do { g_current_test = (name); } while(0)

#define CHECK(cond) do { \
    if (cond) { \
        printf("  PASS  %s: %s\n", g_current_test, #cond); \
        g_pass++; \
    } else { \
        printf("  FAIL  %s: %s  [line %d]\n", g_current_test, #cond, __LINE__); \
        g_fail++; \
    } \
} while(0)

#define CHECK_EQ_INT(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a == _b) { \
        printf("  PASS  %s: %s == %s  (%lld)\n", g_current_test, #a, #b, _a); \
        g_pass++; \
    } else { \
        printf("  FAIL  %s: %s=%lld != %s=%lld  [line %d]\n", \
               g_current_test, #a, _a, #b, _b, __LINE__); \
        g_fail++; \
    } \
} while(0)

#define CHECK_STR(a, b) do { \
    if (strcmp((a),(b)) == 0) { \
        printf("  PASS  %s: \"%s\" == \"%s\"\n", g_current_test, (a), (b)); \
        g_pass++; \
    } else { \
        printf("  FAIL  %s: \"%s\" != \"%s\"  [line %d]\n", g_current_test, (a), (b), __LINE__); \
        g_fail++; \
    } \
} while(0)

// ---------------------------------------------------------------------------
// Helper: build the Upstash envelope {"result":"<escaped_inner>"}
// For testing, just build the JSON inline — we use JSON literals where the
// inner payload is a properly escaped JSON string value.
// ---------------------------------------------------------------------------

// Encode a JSON string into a double-quoted, escaped form and wrap in the
// Upstash envelope {"result": "..."}.  Returns a static buffer — single use.
#include <stdlib.h>

static char *make_envelope(const char *inner_json)
{
    /* Escape inner_json: replace " with backslash-quote, backslash with double-backslash */
    size_t len = strlen(inner_json);
    // Worst case: every char doubled, plus envelope overhead
    char *escaped = malloc(len * 2 + 1);
    size_t eo = 0;
    for (size_t i = 0; i < len; i++) {
        if (inner_json[i] == '\\') { escaped[eo++] = '\\'; escaped[eo++] = '\\'; }
        else if (inner_json[i] == '"') { escaped[eo++] = '\\'; escaped[eo++] = '"'; }
        else { escaped[eo++] = inner_json[i]; }
    }
    escaped[eo] = '\0';

    // Build envelope
    size_t total = eo + 20; // {"result":"..."}
    char *out = malloc(total);
    snprintf(out, total, "{\"result\":\"%s\"}", escaped);
    free(escaped);
    return out; // caller must free
}

// ---------------------------------------------------------------------------
// Test cases
// ---------------------------------------------------------------------------

static void test_valid_v1_payload(void)
{
    TEST("valid_v1_payload");

    // v1 schema: {v,ts,providers:[{id,ok,p,pr,s,sr}]}
    const char *inner =
        "{\"v\":1,\"ts\":\"2024-01-15T12:00:00Z\","
        "\"providers\":["
        "{\"id\":\"claude\",\"ok\":true,\"p\":42.5,\"pr\":\"resets 14:00\","
        "\"s\":10.0,\"sr\":\"daily\"}"
        "]}";

    char *env = make_envelope(inner);
    stats_t st;
    stats_parse_t r = stats_model_parse(env, &st);
    free(env);

    CHECK_EQ_INT(r, STATS_PARSE_OK);
    CHECK_EQ_INT(st.v, 1);
    CHECK_STR(st.ts, "2024-01-15T12:00:00Z");
    CHECK_EQ_INT(st.n, 1);
    CHECK_STR(st.p[0].id, "claude");
    CHECK(st.p[0].ok == true);
    CHECK(st.p[0].primary.has == true);
    CHECK(st.p[0].primary.pct > 42.0f && st.p[0].primary.pct < 43.0f);
    CHECK_STR(st.p[0].primary.reset, "resets 14:00");
    CHECK(st.p[0].secondary.has == true);
    CHECK(st.p[0].secondary.pct > 9.5f && st.p[0].secondary.pct < 10.5f);
    CHECK_STR(st.p[0].secondary.reset, "daily");
    CHECK(st.p[0].has_cost == false);
}

static void test_valid_v2_with_cost(void)
{
    TEST("valid_v2_with_cost");

    // v2 adds a `cost` object; must be backward-compatible
    const char *inner =
        "{\"v\":2,\"ts\":\"2024-02-01T08:00:00Z\","
        "\"providers\":["
        "{\"id\":\"claude\",\"ok\":true,\"p\":55.0,"
        "\"cost\":{\"ct\":199,\"cm\":1500,\"tt\":100000,\"tm\":500000,"
        "\"xu\":50,\"xl\":200,\"cw\":400,\"cr\":300,\"cl\":1000,"
        "\"h\":[10,20,30]}}"
        "]}";

    char *env = make_envelope(inner);
    stats_t st;
    stats_parse_t r = stats_model_parse(env, &st);
    free(env);

    CHECK_EQ_INT(r, STATS_PARSE_OK);
    CHECK_EQ_INT(st.v, 2);
    CHECK_EQ_INT(st.n, 1);
    CHECK(st.p[0].has_cost == true);
    CHECK_EQ_INT(st.p[0].cost_today_c, 199);
    CHECK_EQ_INT(st.p[0].cost_month_c, 1500);
    CHECK_EQ_INT((long long)st.p[0].tok_today, 100000LL);
    CHECK_EQ_INT((long long)st.p[0].tok_month, 500000LL);
    CHECK_EQ_INT(st.p[0].extra_used_c, 50);
    CHECK_EQ_INT(st.p[0].extra_limit_c, 200);
    CHECK_EQ_INT(st.p[0].cost_week_c, 400);
    CHECK_EQ_INT(st.p[0].credits_remaining_c, 300);
    CHECK_EQ_INT(st.p[0].credits_limit_c, 1000);
    CHECK_EQ_INT(st.p[0].hist_n, 3);
    CHECK_EQ_INT(st.p[0].hist[0], 10);
    CHECK_EQ_INT(st.p[0].hist[1], 20);
    CHECK_EQ_INT(st.p[0].hist[2], 30);
}

static void test_moonshot_credit_balance_parsed(void)
{
    TEST("moonshot_credit_balance_parsed");
    const char *inner =
        "{\"v\":2,\"ts\":\"2024-02-01T08:00:00Z\","
        "\"providers\":[{\"id\":\"moonshot\",\"ok\":true,\"cost\":{\"cr\":2213}}]}";

    char *env = make_envelope(inner);
    stats_t st;
    stats_parse_t r = stats_model_parse(env, &st);
    free(env);

    CHECK_EQ_INT(r, STATS_PARSE_OK);
    CHECK(st.p[0].has_cost == true);
    CHECK_EQ_INT(st.p[0].credits_remaining_c, 2213);
}

static void test_result_null_gives_no_data(void)
{
    TEST("result_null_gives_no_data");

    // result is JSON null -> STATS_PARSE_NO_DATA
    const char *body = "{\"result\":null}";
    stats_t st;
    stats_parse_t r = stats_model_parse(body, &st);

    CHECK_EQ_INT(r, STATS_PARSE_NO_DATA);
}

static void test_result_absent_gives_no_data(void)
{
    TEST("result_absent_gives_no_data");

    // No "result" key at all
    const char *body = "{\"other\":42}";
    stats_t st;
    stats_parse_t r = stats_model_parse(body, &st);

    CHECK_EQ_INT(r, STATS_PARSE_NO_DATA);
}

static void test_result_non_string_gives_bad(void)
{
    TEST("result_non_string_gives_bad");

    // result is a number (not string, not null) -> STATS_PARSE_BAD
    const char *body = "{\"result\":42}";
    stats_t st;
    stats_parse_t r = stats_model_parse(body, &st);

    CHECK_EQ_INT(r, STATS_PARSE_BAD);
}

static void test_malformed_outer_json(void)
{
    TEST("malformed_outer_json");

    // Outer envelope is malformed
    const char *body = "{not valid json!!!";
    stats_t st;
    stats_parse_t r = stats_model_parse(body, &st);

    CHECK_EQ_INT(r, STATS_PARSE_BAD);
}

static void test_malformed_inner_json(void)
{
    TEST("malformed_inner_json");

    // Outer is valid, but result string contains malformed JSON
    const char *body = "{\"result\":\"not valid inner json!!!\"}";
    stats_t st;
    stats_parse_t r = stats_model_parse(body, &st);

    // Inner parse fails -> STATS_PARSE_BAD
    CHECK_EQ_INT(r, STATS_PARSE_BAD);
}

static void test_result_empty_string_gives_bad(void)
{
    TEST("result_empty_string_gives_bad");

    // result is an empty string (non-null but empty) -> STATS_PARSE_BAD
    const char *body = "{\"result\":\"\"}";
    stats_t st;
    stats_parse_t r = stats_model_parse(body, &st);

    CHECK_EQ_INT(r, STATS_PARSE_BAD);
}

static void test_unknown_schema_version(void)
{
    TEST("unknown_schema_version_v3");

    // v=3 is not understood: firmware rejects forward-unknown versions
    const char *inner = "{\"v\":3,\"ts\":\"2024-01-01T00:00:00Z\",\"providers\":[]}";
    char *env = make_envelope(inner);
    stats_t st;
    stats_parse_t r = stats_model_parse(env, &st);
    free(env);

    CHECK_EQ_INT(r, STATS_PARSE_BAD);
}

static void test_ok_false_entry_handled(void)
{
    TEST("ok_false_entry_handled");

    // ok:false entry: only id+ok present, no p/s/cost fields
    const char *inner =
        "{\"v\":1,\"ts\":\"2024-01-01T00:00:00Z\","
        "\"providers\":["
        "{\"id\":\"claude\",\"ok\":false}"
        "]}";

    char *env = make_envelope(inner);
    stats_t st;
    stats_parse_t r = stats_model_parse(env, &st);
    free(env);

    CHECK_EQ_INT(r, STATS_PARSE_OK);
    CHECK_EQ_INT(st.n, 1);
    CHECK_STR(st.p[0].id, "claude");
    CHECK(st.p[0].ok == false);
    CHECK(st.p[0].primary.has == false);
    CHECK(st.p[0].secondary.has == false);
    CHECK(st.p[0].has_cost == false);
}

static void test_null_body_gives_bad(void)
{
    TEST("null_body_gives_bad");

    stats_t st;
    stats_parse_t r = stats_model_parse(NULL, &st);
    CHECK_EQ_INT(r, STATS_PARSE_BAD);
}

static void test_null_out_gives_bad(void)
{
    TEST("null_out_gives_bad");

    stats_parse_t r = stats_model_parse("{\"result\":null}", NULL);
    CHECK_EQ_INT(r, STATS_PARSE_BAD);
}

static void test_i32_clamp_huge_positive(void)
{
    TEST("i32_clamp_huge_positive");

    // Feed a number far exceeding INT32_MAX (2^40) in the cost.ct field.
    // Expect it clamps to INT32_MAX = 2147483647.
    const char *inner =
        "{\"v\":2,\"ts\":\"2024-01-01T00:00:00Z\","
        "\"providers\":["
        "{\"id\":\"claude\",\"ok\":true,"
        "\"cost\":{\"ct\":1099511627776}}"   /* 2^40 */
        "]}";

    char *env = make_envelope(inner);
    stats_t st;
    stats_parse_t r = stats_model_parse(env, &st);
    free(env);

    CHECK_EQ_INT(r, STATS_PARSE_OK);
    CHECK_EQ_INT(st.p[0].cost_today_c, INT32_MAX);
}

static void test_i32_clamp_huge_negative(void)
{
    TEST("i32_clamp_huge_negative");

    // Feed a number far below INT32_MIN (-2^40).
    // Expect it clamps to INT32_MIN = -2147483648.
    const char *inner =
        "{\"v\":2,\"ts\":\"2024-01-01T00:00:00Z\","
        "\"providers\":["
        "{\"id\":\"claude\",\"ok\":true,"
        "\"cost\":{\"cm\":-1099511627776}}"   /* -2^40 */
        "]}";

    char *env = make_envelope(inner);
    stats_t st;
    stats_parse_t r = stats_model_parse(env, &st);
    free(env);

    CHECK_EQ_INT(r, STATS_PARSE_OK);
    CHECK_EQ_INT(st.p[0].cost_month_c, INT32_MIN);
}

static void test_i64_clamp_huge_positive(void)
{
    TEST("i64_clamp_huge_positive");

    // Feed a token count beyond INT64_MAX via a very large float.
    // 9.3e18 > 9223372036854775808.0 -> expect INT64_MAX
    const char *inner =
        "{\"v\":2,\"ts\":\"2024-01-01T00:00:00Z\","
        "\"providers\":["
        "{\"id\":\"claude\",\"ok\":true,"
        "\"cost\":{\"tm\":9.3e18}}"
        "]}";

    char *env = make_envelope(inner);
    stats_t st;
    stats_parse_t r = stats_model_parse(env, &st);
    free(env);

    CHECK_EQ_INT(r, STATS_PARSE_OK);
    CHECK_EQ_INT((long long)st.p[0].tok_month, (long long)INT64_MAX);
}

static void test_i64_clamp_huge_negative(void)
{
    TEST("i64_clamp_huge_negative");

    // -9.3e18 < -9223372036854775808.0 -> expect INT64_MIN
    const char *inner =
        "{\"v\":2,\"ts\":\"2024-01-01T00:00:00Z\","
        "\"providers\":["
        "{\"id\":\"claude\",\"ok\":true,"
        "\"cost\":{\"tt\":-9.3e18}}"
        "]}";

    char *env = make_envelope(inner);
    stats_t st;
    stats_parse_t r = stats_model_parse(env, &st);
    free(env);

    CHECK_EQ_INT(r, STATS_PARSE_OK);
    CHECK_EQ_INT((long long)st.p[0].tok_today, (long long)INT64_MIN);
}

static void test_pct_hist_clamp_below_zero(void)
{
    TEST("pct_hist_clamp_below_zero");

    // ph array entry -5 -> clamped to 0
    const char *inner =
        "{\"v\":2,\"ts\":\"2024-01-01T00:00:00Z\","
        "\"providers\":["
        "{\"id\":\"claude\",\"ok\":true,\"ph\":[-5,0,50,100,255]}"
        "]}";

    char *env = make_envelope(inner);
    stats_t st;
    stats_parse_t r = stats_model_parse(env, &st);
    free(env);

    CHECK_EQ_INT(r, STATS_PARSE_OK);
    CHECK_EQ_INT(st.p[0].pct_hist_n, 5);
    CHECK_EQ_INT(st.p[0].pct_hist[0], 0);    // -5 -> 0
    CHECK_EQ_INT(st.p[0].pct_hist[1], 0);    // 0 stays 0
    CHECK_EQ_INT(st.p[0].pct_hist[2], 50);   // 50 unchanged
    CHECK_EQ_INT(st.p[0].pct_hist[3], 100);  // 100 stays 100
    CHECK_EQ_INT(st.p[0].pct_hist[4], 100);  // 255 -> 100
}

static void test_array_cap_providers(void)
{
    TEST("array_cap_providers");

    // Provide STATS_MAX_PROVIDERS+3 providers; only STATS_MAX_PROVIDERS accepted.
    // Build the JSON manually: 15 providers (max is 12).
    const int OVER = STATS_MAX_PROVIDERS + 3; // 15
    char inner[4096];
    int pos = 0;
    pos += snprintf(inner + pos, sizeof(inner) - pos,
                    "{\"v\":1,\"ts\":\"2024-01-01T00:00:00Z\",\"providers\":[");
    for (int i = 0; i < OVER; i++) {
        if (i > 0) pos += snprintf(inner + pos, sizeof(inner) - pos, ",");
        pos += snprintf(inner + pos, sizeof(inner) - pos,
                        "{\"id\":\"p%d\",\"ok\":true}", i);
    }
    pos += snprintf(inner + pos, sizeof(inner) - pos, "]}");

    char *env = make_envelope(inner);
    stats_t st;
    stats_parse_t r = stats_model_parse(env, &st);
    free(env);

    CHECK_EQ_INT(r, STATS_PARSE_OK);
    CHECK_EQ_INT(st.n, STATS_MAX_PROVIDERS);  // capped at 12
}

static void test_array_cap_hist(void)
{
    TEST("array_cap_hist");

    // Provide STATS_HIST_MAX + 5 entries in cost.h; only STATS_HIST_MAX accepted.
    const int OVER = STATS_HIST_MAX + 5; // 36
    char cost_h[1024];
    int pos = 0;
    pos += snprintf(cost_h + pos, sizeof(cost_h) - pos, "[");
    for (int i = 0; i < OVER; i++) {
        if (i > 0) pos += snprintf(cost_h + pos, sizeof(cost_h) - pos, ",");
        pos += snprintf(cost_h + pos, sizeof(cost_h) - pos, "%d", i + 1);
    }
    pos += snprintf(cost_h + pos, sizeof(cost_h) - pos, "]");

    char inner[2048];
    snprintf(inner, sizeof(inner),
             "{\"v\":2,\"ts\":\"2024-01-01T00:00:00Z\","
             "\"providers\":["
             "{\"id\":\"claude\",\"ok\":true,\"cost\":{\"h\":%s}}"
             "]}", cost_h);

    char *env = make_envelope(inner);
    stats_t st;
    stats_parse_t r = stats_model_parse(env, &st);
    free(env);

    CHECK_EQ_INT(r, STATS_PARSE_OK);
    CHECK_EQ_INT(st.p[0].hist_n, STATS_HIST_MAX);  // capped at 31
}

static void test_array_cap_pct_hist(void)
{
    TEST("array_cap_pct_hist");

    // Provide STATS_PCT_HIST_MAX + 5 entries in ph; only STATS_PCT_HIST_MAX accepted.
    const int OVER = STATS_PCT_HIST_MAX + 5; // 29
    char ph[512];
    int pos = 0;
    pos += snprintf(ph + pos, sizeof(ph) - pos, "[");
    for (int i = 0; i < OVER; i++) {
        if (i > 0) pos += snprintf(ph + pos, sizeof(ph) - pos, ",");
        pos += snprintf(ph + pos, sizeof(ph) - pos, "%d", i % 100);
    }
    pos += snprintf(ph + pos, sizeof(ph) - pos, "]");

    char inner[1024];
    snprintf(inner, sizeof(inner),
             "{\"v\":2,\"ts\":\"2024-01-01T00:00:00Z\","
             "\"providers\":["
             "{\"id\":\"claude\",\"ok\":true,\"ph\":%s}"
             "]}", ph);

    char *env = make_envelope(inner);
    stats_t st;
    stats_parse_t r = stats_model_parse(env, &st);
    free(env);

    CHECK_EQ_INT(r, STATS_PARSE_OK);
    CHECK_EQ_INT(st.p[0].pct_hist_n, STATS_PCT_HIST_MAX);  // capped at 24
}

static void test_lm_models_cap(void)
{
    TEST("lm_models_cap");

    // Provide LM_MODELS_MAX + 2 models; only LM_MODELS_MAX accepted.
    const int OVER = 10 + 2; // LM_MODELS_MAX=10, so provide 12
    char models[1024];
    int pos = 0;
    pos += snprintf(models + pos, sizeof(models) - pos, "[");
    for (int i = 0; i < OVER; i++) {
        if (i > 0) pos += snprintf(models + pos, sizeof(models) - pos, ",");
        pos += snprintf(models + pos, sizeof(models) - pos,
                        "{\"id\":\"m%d\",\"rq\":%d}", i, i * 10);
    }
    pos += snprintf(models + pos, sizeof(models) - pos, "]");

    char inner[2048];
    snprintf(inner, sizeof(inner),
             "{\"v\":2,\"ts\":\"2024-01-01T00:00:00Z\","
             "\"providers\":["
             "{\"id\":\"lmstudio\",\"ok\":true,"
             "\"lm\":{\"rq\":100,\"tk\":5000,\"models\":%s}}"
             "]}", models);

    char *env = make_envelope(inner);
    stats_t st;
    stats_parse_t r = stats_model_parse(env, &st);
    free(env);

    CHECK_EQ_INT(r, STATS_PARSE_OK);
    CHECK_EQ_INT(st.p[0].has_lm, 1);
    CHECK_EQ_INT(st.p[0].lm_models_n, 10);  // capped at LM_MODELS_MAX
}

static void test_pi_provider_parsed(void)
{
    TEST("pi_provider_parsed");

    const char *inner =
        "{\"v\":2,\"ts\":\"2024-01-01T00:00:00Z\","
        "\"providers\":["
        "{\"id\":\"pi\",\"ok\":true,\"p\":30.0,"
        "\"pi\":{\"ts\":88,\"tt\":12345,\"ps\":999,\"pt\":67890,"
        "\"h\":[5,10,15],\"ht\":[100,0,200,300]}}"
        "]}";

    char *env = make_envelope(inner);
    stats_t st;
    stats_parse_t r = stats_model_parse(env, &st);
    free(env);

    CHECK_EQ_INT(r, STATS_PARSE_OK);
    CHECK_STR(st.p[0].id, "pi");
    CHECK(st.p[0].has_cost == true);
    CHECK_EQ_INT(st.p[0].cost_today_c, 88);
    CHECK_EQ_INT((long long)st.p[0].tok_today, 12345LL);
    CHECK_EQ_INT(st.p[0].cost_month_c, 999);
    CHECK_EQ_INT((long long)st.p[0].tok_month, 67890LL);
    CHECK_EQ_INT(st.p[0].hist_n, 3);
    CHECK_EQ_INT(st.p[0].hist[0], 5);
    // pi.ht -> dedicated token history (sibling of spend hist[]).
    CHECK_EQ_INT(st.p[0].pi_ht_n, 4);
    CHECK_EQ_INT((long long)st.p[0].pi_ht[0], 100LL);
    CHECK_EQ_INT((long long)st.p[0].pi_ht[1], 0LL);
    CHECK_EQ_INT((long long)st.p[0].pi_ht[2], 200LL);
    CHECK_EQ_INT((long long)st.p[0].pi_ht[3], 300LL);
}

static void test_cursor_provider_parsed(void)
{
    TEST("cursor_provider_parsed");

    const char *inner =
        "{\"v\":2,\"ts\":\"2024-01-01T00:00:00Z\","
        "\"providers\":["
        "{\"id\":\"cursor\",\"ok\":true,"
        "\"cu\":{\"sess\":true,\"tk\":50000,\"mxt\":200000,"
        "\"ht\":[1000,2000,3000]}}"
        "]}";

    char *env = make_envelope(inner);
    stats_t st;
    stats_parse_t r = stats_model_parse(env, &st);
    free(env);

    CHECK_EQ_INT(r, STATS_PARSE_OK);
    CHECK_STR(st.p[0].id, "cursor");
    CHECK(st.p[0].has_cu == true);
    CHECK(st.p[0].cu_sess_ok == true);
    CHECK_EQ_INT((long long)st.p[0].cu_tok_today, 50000LL);
    CHECK_EQ_INT((long long)st.p[0].cu_tok_month_max, 200000LL);
    CHECK_EQ_INT(st.p[0].cu_ht_n, 3);
    CHECK_EQ_INT((long long)st.p[0].cu_ht[2], 3000LL);
}

static void test_cursor_sess_false(void)
{
    TEST("cursor_sess_false");

    const char *inner =
        "{\"v\":2,\"ts\":\"2024-01-01T00:00:00Z\","
        "\"providers\":["
        "{\"id\":\"cursor\",\"ok\":true,"
        "\"cu\":{\"sess\":false,\"tk\":100}}"
        "]}";

    char *env = make_envelope(inner);
    stats_t st;
    stats_parse_t r = stats_model_parse(env, &st);
    free(env);

    CHECK_EQ_INT(r, STATS_PARSE_OK);
    CHECK(st.p[0].cu_sess_ok == false);
}

static void test_opencodego_provider_parsed(void)
{
    TEST("opencodego_provider_parsed");

    const char *inner =
        "{\"v\":2,\"ts\":\"2024-01-01T00:00:00Z\","
        "\"providers\":["
        "{\"id\":\"opencodego\",\"ok\":true,\"p\":45.0,\"pr\":\"5h\","
        "\"s\":30.0,\"sr\":\"weekly\",\"t\":10.0,\"tr\":\"monthly\","
        "\"oc\":{\"tk\":75000,\"ct\":123,\"mxt\":200000,\"ht\":[5000,6000,7000]}}"
        "]}";

    char *env = make_envelope(inner);
    stats_t st;
    stats_parse_t r = stats_model_parse(env, &st);
    free(env);

    CHECK_EQ_INT(r, STATS_PARSE_OK);
    CHECK_STR(st.p[0].id, "opencodego");
    CHECK(st.p[0].has_oc == true);
    CHECK_EQ_INT((long long)st.p[0].oc_tok_today, 75000LL);
    CHECK_EQ_INT(st.p[0].oc_cost_today_c, 123);
    CHECK_EQ_INT((long long)st.p[0].oc_tok_month_max, 200000LL);
    CHECK_EQ_INT(st.p[0].oc_ht_n, 3);
    CHECK_EQ_INT((long long)st.p[0].oc_ht[0], 5000LL);
    CHECK_EQ_INT((long long)st.p[0].oc_ht[1], 6000LL);
    CHECK_EQ_INT((long long)st.p[0].oc_ht[2], 7000LL);
}

static void test_pi_block_ignored_for_non_pi_provider(void)
{
    TEST("pi_block_ignored_for_non_pi_provider");

    // A non-pi provider with a "pi" block: parse_pi should skip it (id check).
    const char *inner =
        "{\"v\":2,\"ts\":\"2024-01-01T00:00:00Z\","
        "\"providers\":["
        "{\"id\":\"claude\",\"ok\":true,"
        "\"pi\":{\"ts\":99,\"tt\":888}}"
        "]}";

    char *env = make_envelope(inner);
    stats_t st;
    stats_parse_t r = stats_model_parse(env, &st);
    free(env);

    CHECK_EQ_INT(r, STATS_PARSE_OK);
    // has_cost must remain false because parse_pi guards on id=="pi"
    CHECK(st.p[0].has_cost == false);
}

static void test_provider_with_no_id_skipped(void)
{
    TEST("provider_with_no_id_skipped");

    // An entry missing "id" should not be counted (p->id[0] == '\0').
    const char *inner =
        "{\"v\":1,\"ts\":\"2024-01-01T00:00:00Z\","
        "\"providers\":["
        "{\"ok\":true},"                          // no id -> skipped
        "{\"id\":\"claude\",\"ok\":true}"         // valid
        "]}";

    char *env = make_envelope(inner);
    stats_t st;
    stats_parse_t r = stats_model_parse(env, &st);
    free(env);

    CHECK_EQ_INT(r, STATS_PARSE_OK);
    CHECK_EQ_INT(st.n, 1);
    CHECK_STR(st.p[0].id, "claude");
}

static void test_multiple_providers(void)
{
    TEST("multiple_providers");

    const char *inner =
        "{\"v\":1,\"ts\":\"2024-01-01T00:00:00Z\","
        "\"providers\":["
        "{\"id\":\"pi\",\"ok\":true,\"p\":10.0},"
        "{\"id\":\"claude\",\"ok\":true,\"p\":20.0},"
        "{\"id\":\"openrouter\",\"ok\":false}"
        "]}";

    char *env = make_envelope(inner);
    stats_t st;
    stats_parse_t r = stats_model_parse(env, &st);
    free(env);

    CHECK_EQ_INT(r, STATS_PARSE_OK);
    CHECK_EQ_INT(st.n, 3);
    CHECK_STR(st.p[0].id, "pi");
    CHECK_STR(st.p[1].id, "claude");
    CHECK_STR(st.p[2].id, "openrouter");
    CHECK(st.p[2].ok == false);
}

// ---------------------------------------------------------------------------
// stats_model_reorder tests
// ---------------------------------------------------------------------------

static void test_reorder_known_providers_sorted(void)
{
    TEST("reorder_known_providers_sorted");

    // Input order is scrambled; Codex/Cursor must follow LM Studio and
    // precede OpenRouter in the canonical summary sequence.
    // Canonical: pi=0, lmstudio=1, claude=2, codex=3, cursor=4, opencodego=5,
    // qwencloud=6, openrouter=7, mimo=8, moonshot=9, deepseek=10
    const char *inner =
        "{\"v\":1,\"ts\":\"2024-01-01T00:00:00Z\","
        "\"providers\":["
        "{\"id\":\"openrouter\",\"ok\":true},"
        "{\"id\":\"pi\",\"ok\":true},"
        "{\"id\":\"cursor\",\"ok\":true},"
        "{\"id\":\"codex\",\"ok\":true},"
        "{\"id\":\"claude\",\"ok\":true},"
        "{\"id\":\"lmstudio\",\"ok\":true}"
        "]}";

    char *env = make_envelope(inner);
    stats_t st;
    stats_model_parse(env, &st);
    free(env);

    stats_model_reorder(&st);

    CHECK_EQ_INT(st.n, 6);
    // Codex/Cursor fall directly after LM Studio and before OpenRouter.
    CHECK_STR(st.p[0].id, "pi");
    CHECK_STR(st.p[1].id, "lmstudio");
    CHECK_STR(st.p[2].id, "claude");
    CHECK_STR(st.p[3].id, "codex");
    CHECK_STR(st.p[4].id, "cursor");
    CHECK_STR(st.p[5].id, "openrouter");
}

static void test_reorder_opencodego_insertion(void)
{
    TEST("reorder_opencodego_insertion");

    // opencodego sorts below cursor; qwencloud sits between opencodego and
    // openrouter, ahead of mimo.
    // Canonical: pi=0, lmstudio=1, claude=2, codex=3, cursor=4, opencodego=5,
    // qwencloud=6, openrouter=7, mimo=8, moonshot=9, deepseek=10
    const char *inner =
        "{\"v\":1,\"ts\":\"2024-01-01T00:00:00Z\","
        "\"providers\":["
        "{\"id\":\"openrouter\",\"ok\":true},"
        "{\"id\":\"lmstudio\",\"ok\":true},"
        "{\"id\":\"opencodego\",\"ok\":true},"
        "{\"id\":\"mimo\",\"ok\":true},"
        "{\"id\":\"qwencloud\",\"ok\":true},"
        "{\"id\":\"pi\",\"ok\":true},"
        "{\"id\":\"cursor\",\"ok\":true}"
        "]}";

    char *env = make_envelope(inner);
    stats_t st;
    stats_model_parse(env, &st);
    free(env);

    stats_model_reorder(&st);

    CHECK_EQ_INT(st.n, 7);
    CHECK_STR(st.p[0].id, "pi");
    CHECK_STR(st.p[1].id, "lmstudio");
    CHECK_STR(st.p[2].id, "cursor");
    CHECK_STR(st.p[3].id, "opencodego");
    CHECK_STR(st.p[4].id, "qwencloud");
    CHECK_STR(st.p[5].id, "openrouter");
    CHECK_STR(st.p[6].id, "mimo");
}

static void test_reorder_ramp_after_deepseek(void)
{
    TEST("reorder_ramp_after_deepseek");

    // ramp slots directly after deepseek, before unlisted providers.
    // Canonical: ... moonshot=9, deepseek=10, ramp=11, ollama=12
    const char *inner =
        "{\"v\":1,\"ts\":\"2024-01-01T00:00:00Z\","
        "\"providers\":["
        "{\"id\":\"ramp\",\"ok\":true},"
        "{\"id\":\"deepseek\",\"ok\":true},"
        "{\"id\":\"moonshot\",\"ok\":true},"
        "{\"id\":\"pi\",\"ok\":true}"
        "]}";

    char *env = make_envelope(inner);
    stats_t st;
    stats_model_parse(env, &st);
    free(env);

    stats_model_reorder(&st);

    CHECK_EQ_INT(st.n, 4);
    CHECK_STR(st.p[0].id, "pi");
    CHECK_STR(st.p[1].id, "moonshot");
    CHECK_STR(st.p[2].id, "deepseek");
    CHECK_STR(st.p[3].id, "ramp");
}

static void test_reorder_unknown_sinks_to_end(void)
{
    TEST("reorder_unknown_sinks_to_end");

    // unknown providers sink after known ones, preserving relative order.
    const char *inner =
        "{\"v\":1,\"ts\":\"2024-01-01T00:00:00Z\","
        "\"providers\":["
        "{\"id\":\"unknown1\",\"ok\":true},"
        "{\"id\":\"claude\",\"ok\":true},"
        "{\"id\":\"unknown2\",\"ok\":true},"
        "{\"id\":\"pi\",\"ok\":true}"
        "]}";

    char *env = make_envelope(inner);
    stats_t st;
    stats_model_parse(env, &st);
    free(env);

    stats_model_reorder(&st);

    CHECK_EQ_INT(st.n, 4);
    CHECK_STR(st.p[0].id, "pi");
    CHECK_STR(st.p[1].id, "claude");
    // unknown1 and unknown2 follow in original relative order
    CHECK_STR(st.p[2].id, "unknown1");
    CHECK_STR(st.p[3].id, "unknown2");
}

static void test_reorder_single_provider_safe(void)
{
    TEST("reorder_single_provider_safe");

    const char *inner =
        "{\"v\":1,\"ts\":\"2024-01-01T00:00:00Z\","
        "\"providers\":["
        "{\"id\":\"claude\",\"ok\":true}"
        "]}";

    char *env = make_envelope(inner);
    stats_t st;
    stats_model_parse(env, &st);
    free(env);

    // Should not crash or corrupt with n=1
    stats_model_reorder(&st);

    CHECK_EQ_INT(st.n, 1);
    CHECK_STR(st.p[0].id, "claude");
}

static void test_reorder_empty_safe(void)
{
    TEST("reorder_empty_safe");

    const char *inner =
        "{\"v\":1,\"ts\":\"2024-01-01T00:00:00Z\","
        "\"providers\":[]}";

    char *env = make_envelope(inner);
    stats_t st;
    stats_model_parse(env, &st);
    free(env);

    // n=0: should return immediately without crash
    stats_model_reorder(&st);

    CHECK_EQ_INT(st.n, 0);
}

static void test_reorder_null_stats_safe(void)
{
    TEST("reorder_null_stats_safe");

    // Passing NULL: should not crash (early return guard)
    stats_model_reorder(NULL);
    // If we reach here without crashing, the guard works.
    g_pass++;
    printf("  PASS  %s: stats_model_reorder(NULL) did not crash\n", g_current_test);
}

static void test_lm_week_cap(void)
{
    TEST("lm_week_cap");

    // Provide LM_WEEK_MAX+2 week entries; only LM_WEEK_MAX=7 accepted.
    const int OVER = 7 + 2; // 9
    char week[1024];
    int pos = 0;
    pos += snprintf(week + pos, sizeof(week) - pos, "[");
    for (int i = 0; i < OVER; i++) {
        if (i > 0) pos += snprintf(week + pos, sizeof(week) - pos, ",");
        pos += snprintf(week + pos, sizeof(week) - pos,
                        "{\"d\":\"0%d-01\",\"rq\":%d,\"tk\":%d,\"cp\":0.5,\"ch\":0.3}",
                        i+1, i*10, i*100);
    }
    pos += snprintf(week + pos, sizeof(week) - pos, "]");

    char inner[2048];
    snprintf(inner, sizeof(inner),
             "{\"v\":2,\"ts\":\"2024-01-01T00:00:00Z\","
             "\"providers\":["
             "{\"id\":\"lmstudio\",\"ok\":true,"
             "\"lm\":{\"rq\":50,\"week\":%s}}"
             "]}", week);

    char *env = make_envelope(inner);
    stats_t st;
    stats_parse_t r = stats_model_parse(env, &st);
    free(env);

    CHECK_EQ_INT(r, STATS_PARSE_OK);
    CHECK_EQ_INT(st.p[0].has_lm, 1);
    CHECK_EQ_INT(st.p[0].lm_week_n, 7);  // capped at LM_WEEK_MAX
}

static void test_lm_hist_caps(void)
{
    TEST("lm_hist_caps");

    // Provide STATS_HIST_MAX+4 entries in lm.hr and lm.ht arrays.
    const int OVER = STATS_HIST_MAX + 4; // 35
    char arr[1024];
    int pos = 0;
    pos += snprintf(arr + pos, sizeof(arr) - pos, "[");
    for (int i = 0; i < OVER; i++) {
        if (i > 0) pos += snprintf(arr + pos, sizeof(arr) - pos, ",");
        pos += snprintf(arr + pos, sizeof(arr) - pos, "%d", i);
    }
    pos += snprintf(arr + pos, sizeof(arr) - pos, "]");

    char inner[4096];
    snprintf(inner, sizeof(inner),
             "{\"v\":2,\"ts\":\"2024-01-01T00:00:00Z\","
             "\"providers\":["
             "{\"id\":\"lmstudio\",\"ok\":true,"
             "\"lm\":{\"rq\":10,\"hr\":%s,\"ht\":%s}}"
             "]}", arr, arr);

    char *env = make_envelope(inner);
    stats_t st;
    stats_parse_t r = stats_model_parse(env, &st);
    free(env);

    CHECK_EQ_INT(r, STATS_PARSE_OK);
    CHECK_EQ_INT(st.p[0].lm_hr_n, STATS_HIST_MAX);
    CHECK_EQ_INT(st.p[0].lm_ht_n, STATS_HIST_MAX);
}

static void test_cu_hist_cap(void)
{
    TEST("cu_hist_cap");

    // Provide STATS_HIST_MAX+3 entries in cu.ht; only STATS_HIST_MAX accepted.
    const int OVER = STATS_HIST_MAX + 3;
    char arr[1024];
    int pos = 0;
    pos += snprintf(arr + pos, sizeof(arr) - pos, "[");
    for (int i = 0; i < OVER; i++) {
        if (i > 0) pos += snprintf(arr + pos, sizeof(arr) - pos, ",");
        pos += snprintf(arr + pos, sizeof(arr) - pos, "%d", i * 100);
    }
    pos += snprintf(arr + pos, sizeof(arr) - pos, "]");

    char inner[2048];
    snprintf(inner, sizeof(inner),
             "{\"v\":2,\"ts\":\"2024-01-01T00:00:00Z\","
             "\"providers\":["
             "{\"id\":\"cursor\",\"ok\":true,"
             "\"cu\":{\"tk\":500,\"ht\":%s}}"
             "]}", arr);

    char *env = make_envelope(inner);
    stats_t st;
    stats_parse_t r = stats_model_parse(env, &st);
    free(env);

    CHECK_EQ_INT(r, STATS_PARSE_OK);
    CHECK_EQ_INT(st.p[0].cu_ht_n, STATS_HIST_MAX);
}

static void test_cost_tok_hist(void)
{
    TEST("cost_tok_hist");

    // OpenRouter publishes tokens alongside a $0 spend history: cost.ht must
    // populate tok_hist[] without disturbing the sibling spend history in h.
    const char *inner =
        "{\"v\":2,\"ts\":\"2024-01-01T00:00:00Z\","
        "\"providers\":["
        "{\"id\":\"openrouter\",\"ok\":true,"
        "\"cost\":{\"tt\":60769529,\"cr\":4562,"
        "\"h\":[0,0,0],\"ht\":[469311,0,60769529]}}"
        "]}";

    char *env = make_envelope(inner);
    stats_t st;
    stats_parse_t r = stats_model_parse(env, &st);
    free(env);

    CHECK_EQ_INT(r, STATS_PARSE_OK);
    CHECK(st.p[0].has_cost == true);
    CHECK(st.p[0].tok_today == 60769529);
    CHECK_EQ_INT(st.p[0].tok_hist_n, 3);
    CHECK(st.p[0].tok_hist[0] == 469311);
    CHECK(st.p[0].tok_hist[2] == 60769529);
    CHECK_EQ_INT(st.p[0].hist_n, 3);
}

static void test_cost_tok_hist_absent(void)
{
    TEST("cost_tok_hist_absent");

    // Providers that never publish cost.ht must leave tok_hist_n at 0 so the
    // UI keeps hiding the token chart for them.
    const char *inner =
        "{\"v\":2,\"ts\":\"2024-01-01T00:00:00Z\","
        "\"providers\":["
        "{\"id\":\"moonshot\",\"ok\":true,\"cost\":{\"cr\":1000,\"h\":[1,2]}}"
        "]}";

    char *env = make_envelope(inner);
    stats_t st;
    stats_parse_t r = stats_model_parse(env, &st);
    free(env);

    CHECK_EQ_INT(r, STATS_PARSE_OK);
    CHECK_EQ_INT(st.p[0].tok_hist_n, 0);
    CHECK_EQ_INT(st.p[0].hist_n, 2);
}

static void test_deepseek_tokens_and_spend(void)
{
    TEST("deepseek_tokens_and_spend");

    // deepseek-stats.sh patches tt/ht/ct/cw onto the balance (cr) that
    // codexbar-stats.sh already scraped. The generic cost parser must absorb
    // all of it with no per-provider handling, and cr must survive alongside
    // the new fields -- ui_render_card keys the balance layout off it.
    const char *inner =
        "{\"v\":2,\"ts\":\"2024-01-01T00:00:00Z\","
        "\"providers\":["
        "{\"id\":\"deepseek\",\"ok\":true,"
        "\"cost\":{\"cr\":4075,\"tt\":1000,\"ct\":43,\"cw\":53,"
        "\"ht\":[0,50,1000]}}"
        "]}";

    char *env = make_envelope(inner);
    stats_t st;
    stats_parse_t r = stats_model_parse(env, &st);
    free(env);

    CHECK_EQ_INT(r, STATS_PARSE_OK);
    CHECK(st.p[0].has_cost == true);
    CHECK_EQ_INT(st.p[0].credits_remaining_c, 4075);
    CHECK(st.p[0].tok_today == 1000);
    CHECK_EQ_INT(st.p[0].cost_today_c, 43);
    CHECK_EQ_INT(st.p[0].cost_week_c, 53);
    CHECK_EQ_INT(st.p[0].tok_hist_n, 3);
    CHECK(st.p[0].tok_hist[2] == 1000);
    // No cost.h published for DeepSeek: the balance card charts tok_hist only.
    CHECK_EQ_INT(st.p[0].hist_n, 0);
}

static void test_cost_tok_hist_cap(void)
{
    TEST("cost_tok_hist_cap");

    const int OVER = STATS_HIST_MAX + 3;
    char arr[1024];
    int pos = 0;
    pos += snprintf(arr + pos, sizeof(arr) - pos, "[");
    for (int i = 0; i < OVER; i++) {
        if (i > 0) pos += snprintf(arr + pos, sizeof(arr) - pos, ",");
        pos += snprintf(arr + pos, sizeof(arr) - pos, "%d", i * 1000);
    }
    pos += snprintf(arr + pos, sizeof(arr) - pos, "]");

    char inner[2048];
    snprintf(inner, sizeof(inner),
             "{\"v\":2,\"ts\":\"2024-01-01T00:00:00Z\","
             "\"providers\":["
             "{\"id\":\"openrouter\",\"ok\":true,\"cost\":{\"ht\":%s}}"
             "]}", arr);

    char *env = make_envelope(inner);
    stats_t st;
    stats_parse_t r = stats_model_parse(env, &st);
    free(env);

    CHECK_EQ_INT(r, STATS_PARSE_OK);
    CHECK_EQ_INT(st.p[0].tok_hist_n, STATS_HIST_MAX);
}

static void test_tertiary_field_parsed(void)
{
    TEST("tertiary_field_t_and_tr_parsed");

    const char *inner =
        "{\"v\":2,\"ts\":\"2024-01-01T00:00:00Z\","
        "\"providers\":["
        "{\"id\":\"openrouter\",\"ok\":true,\"p\":55.0,\"t\":77.5,\"tr\":\"next month\"}"
        "]}";

    char *env = make_envelope(inner);
    stats_t st;
    stats_parse_t r = stats_model_parse(env, &st);
    free(env);

    CHECK_EQ_INT(r, STATS_PARSE_OK);
    CHECK(st.p[0].tertiary.has == true);
    CHECK(st.p[0].tertiary.pct > 77.0f && st.p[0].tertiary.pct < 78.0f);
    CHECK_STR(st.p[0].tertiary.reset, "next month");
}

static void test_pi_hist_resets_on_pi_block(void)
{
    TEST("pi_hist_resets_on_pi_block");

    // If a pi provider has both cost.h and pi.h, parse_pi resets hist_n to 0
    // and owns the chart — pi.h wins.
    const char *inner =
        "{\"v\":2,\"ts\":\"2024-01-01T00:00:00Z\","
        "\"providers\":["
        "{\"id\":\"pi\",\"ok\":true,"
        "\"cost\":{\"h\":[1,2,3]},"
        "\"pi\":{\"ts\":10,\"h\":[99,88]}}"
        "]}";

    char *env = make_envelope(inner);
    stats_t st;
    stats_parse_t r = stats_model_parse(env, &st);
    free(env);

    CHECK_EQ_INT(r, STATS_PARSE_OK);
    // parse_cost runs first (populates hist with 3 entries from cost.h),
    // then parse_pi resets hist_n=0 and repopulates from pi.h
    CHECK_EQ_INT(st.p[0].hist_n, 2);
    CHECK_EQ_INT(st.p[0].hist[0], 99);
    CHECK_EQ_INT(st.p[0].hist[1], 88);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(void)
{
    printf("=== stats_model host unit tests ===\n\n");

    test_valid_v1_payload();
    test_valid_v2_with_cost();
    test_moonshot_credit_balance_parsed();
    test_result_null_gives_no_data();
    test_result_absent_gives_no_data();
    test_result_non_string_gives_bad();
    test_malformed_outer_json();
    test_malformed_inner_json();
    test_result_empty_string_gives_bad();
    test_unknown_schema_version();
    test_ok_false_entry_handled();
    test_null_body_gives_bad();
    test_null_out_gives_bad();
    test_i32_clamp_huge_positive();
    test_i32_clamp_huge_negative();
    test_i64_clamp_huge_positive();
    test_i64_clamp_huge_negative();
    test_pct_hist_clamp_below_zero();
    test_array_cap_providers();
    test_array_cap_hist();
    test_array_cap_pct_hist();
    test_lm_models_cap();
    test_pi_provider_parsed();
    test_cursor_provider_parsed();
    test_cursor_sess_false();
    test_opencodego_provider_parsed();
    test_pi_block_ignored_for_non_pi_provider();
    test_provider_with_no_id_skipped();
    test_multiple_providers();
    test_reorder_known_providers_sorted();
    test_reorder_opencodego_insertion();
    test_reorder_ramp_after_deepseek();
    test_reorder_unknown_sinks_to_end();
    test_reorder_single_provider_safe();
    test_reorder_empty_safe();
    test_reorder_null_stats_safe();
    test_lm_week_cap();
    test_lm_hist_caps();
    test_cu_hist_cap();
    test_cost_tok_hist();
    test_cost_tok_hist_absent();
    test_deepseek_tokens_and_spend();
    test_cost_tok_hist_cap();
    test_tertiary_field_parsed();
    test_pi_hist_resets_on_pi_block();

    printf("\n=== RESULTS: %d passed, %d failed ===\n", g_pass, g_fail);
    return (g_fail > 0) ? 1 : 0;
}
