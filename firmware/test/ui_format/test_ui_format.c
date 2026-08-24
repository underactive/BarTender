// Host-compiled unit tests for firmware/main/ui_format.c — the pure formatting,
// number, provider-resolution and color helpers (Functional Core). Compiles the
// REAL ui_format.c against a minimal lvgl/freertos shim; no LVGL widget globals
// or `st` are touched by any function under test.
//
// This is the render-helper safety net flagged by the Fowler audit (#3): it
// gives the ui_render.c extractions something to regress against, since the
// layout dispatchers reuse exactly these helpers.
//
// Usage: cd firmware/test/ui_format && make run
#include "ui_internal.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, ...) do {                       \
    if (cond) { g_pass++; printf("  PASS  "); }     \
    else      { g_fail++; printf("  FAIL  "); }     \
    printf(__VA_ARGS__); printf("\n");              \
} while (0)

#define EQ_INT(a, b, label)  CHECK((long)(a) == (long)(b), "%s: %ld == %ld", label, (long)(a), (long)(b))
#define EQ_STR(a, b, label)  CHECK(strcmp((a), (b)) == 0, "%s: \"%s\" == \"%s\"", label, (a), (b))

static bool color_eq(lv_color_t c, uint32_t hex)
{
    return c.red == ((hex >> 16) & 0xff) && c.green == ((hex >> 8) & 0xff) && c.blue == (hex & 0xff);
}

static void test_clampi(void)
{
    EQ_INT(clampi(-5, 0, 100), 0,   "clampi: below lo");
    EQ_INT(clampi(150, 0, 100), 100, "clampi: above hi");
    EQ_INT(clampi(50, 0, 100), 50,  "clampi: in range");
    EQ_INT(clampi(0, 0, 100), 0,    "clampi: at lo");
    EQ_INT(clampi(100, 0, 100), 100, "clampi: at hi");
}

static void test_bar_fill(void)
{
    // UI_BAR_INVERT_DEFAULT is true: source usage becomes remaining headroom.
    EQ_INT(bar_fill(0), 100,   "bar_fill: zero used -> full");
    EQ_INT(bar_fill(50), 50,   "bar_fill: midpoint");
    EQ_INT(bar_fill(100), 0,   "bar_fill: fully used -> empty");
    EQ_INT(bar_fill(-5), 100,  "bar_fill: clamp low");
    EQ_INT(bar_fill(150), 0,   "bar_fill: clamp high");
}

static void test_pct_remaining(void)
{
    EQ_INT(pct_remaining_tenths(45.3f), 547, "remaining tenths: 45.3 used -> 54.7");
    EQ_INT(pct_remaining_tenths(0.0f), 1000, "remaining tenths: zero used -> 100.0");
    EQ_INT(pct_remaining_tenths(100.0f), 0, "remaining tenths: 100 used -> 0.0");
    EQ_INT(pct_remaining_tenths(125.3f), 0, "remaining tenths: overage -> 0.0");
    EQ_INT(pct_remaining_tenths(-5.0f), 1000, "remaining tenths: negative -> 100.0");
    EQ_INT(pct_remaining_int(45.3f), 55, "remaining int: 45.3 used -> 55");
    EQ_INT(pct_remaining_int(0.0f), 100, "remaining int: zero used -> 100");
    EQ_INT(pct_remaining_int(125.3f), 0, "remaining int: overage -> 0");
}

static void test_bar_should_pulse(void)
{
    CHECK(bar_should_pulse(100.0f) == false, "bar_should_pulse: at 100 -> no");
    CHECK(bar_should_pulse(100.1f) == true,  "bar_should_pulse: over 100 -> yes");
    CHECK(bar_should_pulse(125.3f) == true,  "bar_should_pulse: overage -> yes");
    CHECK(bar_should_pulse(90.0f) == false,  "bar_should_pulse: at 90 -> no");
}

static void test_bar_pulse_uses_color_cycle(void)
{
    CHECK(bar_pulse_uses_color_cycle("pi") == true,  "color_cycle: pi");
    CHECK(bar_pulse_uses_color_cycle("cursor") == false, "color_cycle: cursor");
    CHECK(bar_pulse_uses_color_cycle(NULL) == false, "color_cycle: null");
}

static void test_pct_tenths(void)
{
    EQ_INT(pct_tenths(false, 50.0f), -1, "pct_tenths: no data -> -1");
    EQ_INT(pct_tenths(true, 45.3f), 453, "pct_tenths: 45.3 -> 453");
    EQ_INT(pct_tenths(true, 0.0f), 0,    "pct_tenths: 0 -> 0");
    EQ_INT(pct_tenths(true, 250.0f), 2500, "pct_tenths: allows >100%");
}

static void test_extra_pct(void)
{
    stats_provider_t p;
    memset(&p, 0, sizeof p);
    EQ_INT(extra_pct(&p), 0, "extra_pct: no cost -> 0");
    p.has_cost = true; p.extra_limit_c = 0;
    EQ_INT(extra_pct(&p), 0, "extra_pct: zero limit -> 0");
    p.extra_limit_c = 1000; p.extra_used_c = 250;
    EQ_INT(extra_pct(&p), 25, "extra_pct: 250/1000 -> 25");
    p.extra_used_c = 5000;
    EQ_INT(extra_pct(&p), 100, "extra_pct: overage clamps to 100");
}

static void test_provider_kind(void)
{
    EQ_INT(provider_kind("pi"), PK_PI,                 "provider_kind: pi");
    EQ_INT(provider_kind("claude"), PK_CLAUDE,         "provider_kind: claude");
    EQ_INT(provider_kind("codex"), PK_CODEX,           "provider_kind: codex");
    EQ_INT(provider_kind("lmstudio"), PK_LMSTUDIO,     "provider_kind: lmstudio");
    EQ_INT(provider_kind("cursor"), PK_CURSOR,         "provider_kind: cursor");
    EQ_INT(provider_kind("openrouter"), PK_OPENROUTER, "provider_kind: openrouter");
    EQ_INT(provider_kind("moonshot"), PK_MOONSHOT,     "provider_kind: moonshot");
    EQ_INT(provider_kind("deepseek"), PK_DEEPSEEK,     "provider_kind: deepseek");
    EQ_INT(provider_kind("nope"), PK_UNKNOWN,          "provider_kind: unknown");
    EQ_INT(provider_kind(NULL), PK_UNKNOWN,            "provider_kind: NULL");
    EQ_INT(provider_kind(""), PK_UNKNOWN,              "provider_kind: empty");
    CHECK(provider_pct_is_baseline(PK_PI) == true, "provider_pct: Pi baseline");
    CHECK(provider_pct_is_baseline(PK_MIMO) == true, "provider_pct: MiMo baseline");
    CHECK(provider_pct_is_baseline(PK_LMSTUDIO) == true, "provider_pct: LM baseline");
    CHECK(provider_pct_is_baseline(PK_CLAUDE) == false, "provider_pct: Claude quota");
}

static void test_summary_provider_name(void)
{
    EQ_STR(summary_provider_name("pi"), "Pi",                 "name: pi");
    EQ_STR(summary_provider_name("lmstudio"), "LM Studio",    "name: lmstudio");
    EQ_STR(summary_provider_name("openrouter"), "OpenRouter", "name: openrouter");
    EQ_STR(summary_provider_name("claude"), "Claude",         "name: claude");
    EQ_STR(summary_provider_name("codex"), "Codex",           "name: codex");
    EQ_STR(summary_provider_name("cursor"), "Cursor",         "name: cursor");
    EQ_STR(summary_provider_name("moonshot"), "Moonshot",     "name: moonshot");
    EQ_STR(summary_provider_name("deepseek"), "DeepSeek",     "name: deepseek");
    EQ_STR(summary_provider_name("xyz"), "xyz",               "name: unknown -> id");
}

static void test_balance_helpers(void)
{
    // Every balance bar is a fixed $0-100 gauge split into quarters ($25 each).
    EQ_INT(balance_bar_units(0, BALANCE_SEG_MAX), 0, "balance units: $0");
    EQ_INT(balance_bar_units(2200, BALANCE_SEG_MAX), 88, "balance units: $22");
    EQ_INT(balance_bar_units(2500, BALANCE_SEG_MAX), 100, "balance units: $25 -> 1 quarter");
    EQ_INT(balance_bar_units(5000, BALANCE_SEG_MAX), 200, "balance units: $50 -> 2 quarters");
    EQ_INT(balance_bar_units(10000, BALANCE_SEG_MAX), 400, "balance units: $100 -> full");
    EQ_INT(balance_bar_units(20000, BALANCE_SEG_MAX), 400, "balance units: cap");

    // $100 circles: ceil(balance/$100) - 1 (exact multiples prefer a full bar).
    EQ_INT(balance_circle_count(0), 0, "circles: $0 -> 0");
    EQ_INT(balance_circle_count(-100), 0, "circles: negative -> 0");
    EQ_INT(balance_circle_count(9999), 0, "circles: $99.99 -> 0");
    EQ_INT(balance_circle_count(10000), 0, "circles: $100 -> 0");
    EQ_INT(balance_circle_count(10001), 1, "circles: $100.01 -> 1");
    EQ_INT(balance_circle_count(15000), 1, "circles: $150 -> 1");
    EQ_INT(balance_circle_count(20000), 1, "circles: $200 -> 1");
    EQ_INT(balance_circle_count(30000), 2, "circles: $300 -> 2");
    EQ_INT(balance_circle_count(61195), 6, "circles: $611.95 -> 6");
    // Remaining $0-100 window after the whole hundreds.
    EQ_INT(balance_window_c(15000, 1), 5000, "window: $150 -> $50");
    EQ_INT(balance_window_c(30000, 2), 10000, "window: $300 -> full");
    EQ_INT(balance_window_c(10000, 0), 10000, "window: $100 -> full");
    EQ_INT(balance_window_c(61195, 6), 1195, "window: $611.95 -> $11.95");

    stats_provider_t p;
    int32_t balance = 0;
    memset(&p, 0, sizeof p); strcpy(p.id, "openrouter");
    p.has_cost = true; p.credits_remaining_c = 2213;
    CHECK(provider_balance_c(&p, &balance) && balance == 2213, "balance provider: OpenRouter");
    memset(&p, 0, sizeof p); strcpy(p.id, "mimo");
    p.has_mo = true; p.mo_balance_c = 1800;
    CHECK(provider_balance_c(&p, &balance) && balance == 1800, "balance provider: MiMo");
    memset(&p, 0, sizeof p); strcpy(p.id, "moonshot");
    p.has_cost = true; p.credits_remaining_c = 9200;
    CHECK(provider_balance_c(&p, &balance) && balance == 9200, "balance provider: Moonshot");
    memset(&p, 0, sizeof p); strcpy(p.id, "claude");
    p.has_cost = true; p.credits_remaining_c = 500;
    CHECK(!provider_balance_c(&p, &balance), "balance provider: Claude excluded");
    memset(&p, 0, sizeof p); strcpy(p.id, "openrouter");
    CHECK(!provider_balance_c(&p, &balance), "balance provider: no cost");
}

static void test_fmt_money(void)
{
    char b[16];
    fmt_money(b, sizeof b, 1247); EQ_STR(b, "$12.47", "fmt_money: 1247");
    fmt_money(b, sizeof b, 0);    EQ_STR(b, "$0.00",  "fmt_money: 0");
    fmt_money(b, sizeof b, 5);    EQ_STR(b, "$0.05",  "fmt_money: 5c");
    fmt_money(b, sizeof b, -1);   EQ_STR(b, "$?",     "fmt_money: negative -> $?");
}

static void test_fmt_pct(void)
{
    char b[16];
    fmt_pct(b, sizeof b, false, 50.0f); EQ_STR(b, "--",     "fmt_pct: no data");
    fmt_pct(b, sizeof b, true, 45.3f);  EQ_STR(b, "54.7%",  "fmt_pct: remaining from 45.3 used");
    fmt_pct(b, sizeof b, true, 100.0f); EQ_STR(b, "0.0%",   "fmt_pct: fully used");
    fmt_pct(b, sizeof b, true, 125.3f); EQ_STR(b, "0.0%",   "fmt_pct: overage");
    fmt_pct(b, sizeof b, true, 0.0f);   EQ_STR(b, "100.0%", "fmt_pct: zero used");
    fmt_pct_used(b, sizeof b, true, 45.3f); EQ_STR(b, "45.3%", "fmt_pct_used: 45.3");
    fmt_pct_used(b, sizeof b, true, 133.3f); EQ_STR(b, "133.3%", "fmt_pct_used: overage");
}

static void test_fmt_tokens(void)
{
    char b[16];
    fmt_tokens(b, sizeof b, 999);        EQ_STR(b, "999",   "fmt_tokens: 999");
    fmt_tokens(b, sizeof b, 1500);       EQ_STR(b, "1.5K",  "fmt_tokens: 1.5K");
    fmt_tokens(b, sizeof b, 1500000);    EQ_STR(b, "1.5M",  "fmt_tokens: 1.5M");
    fmt_tokens(b, sizeof b, 1500000000LL); EQ_STR(b, "1.50B", "fmt_tokens: 1.50B");
    fmt_tokens(b, sizeof b, -1);         EQ_STR(b, "?",     "fmt_tokens: negative");
}

static void test_fmt_tokens_full(void)
{
    char b[32];
    fmt_tokens_full(b, sizeof b, 76234567); EQ_STR(b, "76,234,567", "fmt_tokens_full: grouping");
    fmt_tokens_full(b, sizeof b, 0);        EQ_STR(b, "0",          "fmt_tokens_full: 0");
    fmt_tokens_full(b, sizeof b, 999);      EQ_STR(b, "999",        "fmt_tokens_full: <1000");
}

static void test_up_id(void)
{
    char b[8];
    up_id(b, sizeof b, "claude"); EQ_STR(b, "CLAUDE", "up_id: claude");
    up_id(b, sizeof b, "pi");     EQ_STR(b, "PI",     "up_id: pi");
    up_id(b, sizeof b, "openrouter"); EQ_STR(b, "OPENROU", "up_id: truncates to n-1 (7 chars in 8B buf)");
}

static void test_is_hidden_provider(void)
{
    CHECK(is_hidden_provider("ollama") == false, "is_hidden: ollama visible");
    CHECK(is_hidden_provider("opencode") == true, "is_hidden: opencode hidden");
    CHECK(is_hidden_provider("claude") == false,  "is_hidden: claude visible");
}

static void test_provider_has_limits_card(void)
{
    stats_provider_t p;
    memset(&p, 0, sizeof p);
    CHECK(provider_has_limits_card(&p) == false, "limits_card: empty -> false");
    p.primary.has = true;
    CHECK(provider_has_limits_card(&p) == true,  "limits_card: primary tier -> true");
    memset(&p, 0, sizeof p); p.pct_hist_n = 3;
    CHECK(provider_has_limits_card(&p) == true,  "limits_card: pct_hist -> true");
    memset(&p, 0, sizeof p); p.has_cost = true; p.extra_limit_c = 500;
    CHECK(provider_has_limits_card(&p) == true,  "limits_card: extra budget -> true");
}

static void test_i64_hist_to_i32(void)
{
    int64_t src[3] = { 10, (int64_t)INT32_MAX + 5, 20 };
    int32_t dst[3] = { 0 };
    i64_hist_to_i32(dst, src, 3);
    EQ_INT(dst[0], 10,        "i64->i32: passthrough");
    EQ_INT(dst[1], INT32_MAX, "i64->i32: clamps overflow");
    EQ_INT(dst[2], 20,        "i64->i32: passthrough");
}

static void test_provider_tok_today(void)
{
    stats_provider_t p;
    memset(&p, 0, sizeof p);
    strcpy(p.id, "lmstudio"); p.has_lm = true; p.lm_tok_today = 1234;
    EQ_INT(provider_tok_today(&p), 1234, "tok_today: lmstudio");
    memset(&p, 0, sizeof p);
    strcpy(p.id, "cursor"); p.has_cu = true; p.cu_tok_today = 5678;
    EQ_INT(provider_tok_today(&p), 5678, "tok_today: cursor");
    memset(&p, 0, sizeof p);
    strcpy(p.id, "claude"); p.has_cost = true; p.tok_today = 42;
    EQ_INT(provider_tok_today(&p), 42, "tok_today: default cost path");
}

static void test_provider_avg_bar(void)
{
    stats_provider_t p;
    float pct = -1.0f;
    bool r;

    // MiMo: today=200, history [100,0,200] -> nonzero avg=150 -> 133.3%
    memset(&p, 0, sizeof p); strcpy(p.id, "mimo"); p.has_mo = true;
    p.mo_tok_today = 200; p.mo_ht[0]=100; p.mo_ht[1]=0; p.mo_ht[2]=200; p.mo_ht_n=3;
    r = provider_avg_bar(&p, &pct);
    CHECK(r == true, "avg_bar: mimo -> true");
    EQ_INT((int)(pct*10.0f + 0.5f), 1333, "avg_bar: mimo 200/150 -> 133.3%%");

    // LM Studio: today=50, history [50,50,50] -> avg=50 -> 100.0%
    memset(&p, 0, sizeof p); strcpy(p.id, "lmstudio"); p.has_lm = true;
    p.lm_tok_today = 50; p.lm_ht[0]=50; p.lm_ht[1]=50; p.lm_ht[2]=50; p.lm_ht_n=3;
    r = provider_avg_bar(&p, &pct);
    CHECK(r == true, "avg_bar: lmstudio -> true");
    EQ_INT((int)(pct*10.0f + 0.5f), 1000, "avg_bar: lmstudio 50/50 -> 100.0%%");

    // Pi: today=200 (tok_today), history [0,100,200] -> nonzero avg=150 -> 133.3%
    memset(&p, 0, sizeof p); strcpy(p.id, "pi");
    p.tok_today = 200; p.pi_ht[0]=0; p.pi_ht[1]=100; p.pi_ht[2]=200; p.pi_ht_n=3;
    r = provider_avg_bar(&p, &pct);
    CHECK(r == true, "avg_bar: pi -> true");
    EQ_INT((int)(pct*10.0f + 0.5f), 1333, "avg_bar: pi 200/150 -> 133.3%%");

    // All-zero history -> no baseline -> false
    memset(&p, 0, sizeof p); strcpy(p.id, "mimo"); p.has_mo = true;
    p.mo_tok_today = 200; p.mo_ht[0]=0; p.mo_ht[1]=0; p.mo_ht_n=2;
    r = provider_avg_bar(&p, &pct);
    CHECK(r == false, "avg_bar: all-zero history -> false");

    // Empty history (n=0) -> false
    memset(&p, 0, sizeof p); strcpy(p.id, "mimo"); p.has_mo = true; p.mo_tok_today = 200;
    r = provider_avg_bar(&p, &pct);
    CHECK(r == false, "avg_bar: empty history -> false");

    // today=0 with nonzero history -> 0.0% (still has data)
    memset(&p, 0, sizeof p); strcpy(p.id, "mimo"); p.has_mo = true;
    p.mo_tok_today = 0; p.mo_ht[0]=100; p.mo_ht[1]=200; p.mo_ht_n=2;
    r = provider_avg_bar(&p, &pct);
    CHECK(r == true, "avg_bar: idle today -> true");
    EQ_INT((int)(pct*10.0f + 0.5f), 0, "avg_bar: idle today -> 0.0%%");

    // Missing block flag (has_mo/has_lm false) -> false
    memset(&p, 0, sizeof p); strcpy(p.id, "mimo"); p.mo_tok_today = 200; p.mo_ht_n=1; p.mo_ht[0]=100;
    r = provider_avg_bar(&p, &pct);
    CHECK(r == false, "avg_bar: mimo no has_mo -> false");
    memset(&p, 0, sizeof p); strcpy(p.id, "lmstudio"); p.lm_tok_today = 200; p.lm_ht_n=1; p.lm_ht[0]=100;
    r = provider_avg_bar(&p, &pct);
    CHECK(r == false, "avg_bar: lmstudio no has_lm -> false");

    // Unknown provider -> false (falls back to windowed primary.pct)
    memset(&p, 0, sizeof p); strcpy(p.id, "claude"); p.has_cost = true; p.tok_today = 200;
    r = provider_avg_bar(&p, &pct);
    CHECK(r == false, "avg_bar: claude -> false");
}

static void test_colors(void)
{
    CHECK(color_eq(pct_color(95.0f), 0xe5484d), "pct_color: >=90 red");
    CHECK(color_eq(pct_color(70.0f), 0xf5a623), "pct_color: >=60 amber");
    CHECK(color_eq(pct_color(30.0f), 0x30c14e), "pct_color: <60 green");

    // bar_color: provider with a registered accent uses it (claude = 0xCC7C5E);
    // an unknown provider falls back to the pct_color usage ramp.
    stats_provider_t p;
    memset(&p, 0, sizeof p); strcpy(p.id, "claude");
    CHECK(color_eq(bar_color(&p, 30.0f), 0xCC7C5E), "bar_color: claude accent");
    memset(&p, 0, sizeof p); strcpy(p.id, "totallyunknown");
    CHECK(color_eq(bar_color(&p, 95.0f), 0xe5484d), "bar_color: unknown -> ramp");
}

int main(void)
{
    test_clampi();
    test_bar_fill();
    test_pct_remaining();
    test_bar_should_pulse();
    test_bar_pulse_uses_color_cycle();
    test_pct_tenths();
    test_extra_pct();
    test_provider_kind();
    test_summary_provider_name();
    test_balance_helpers();
    test_fmt_money();
    test_fmt_pct();
    test_fmt_tokens();
    test_fmt_tokens_full();
    test_up_id();
    test_is_hidden_provider();
    test_provider_has_limits_card();
    test_i64_hist_to_i32();
    test_provider_tok_today();
    test_provider_avg_bar();
    test_colors();

    printf("\n=== RESULTS: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
