// firmware/main/stats_model.h
//
// Two-step parse of the Upstash response into a flat struct.
//   body  = {"result":"<escaped-json>"}                       (Upstash envelope)
//   inner = {v,ts,providers:[{id,ok,p?,pr?,s?,sr?,cost?,pi?}]} (our payload)
// Field optionality matches the live contract verified against real bytes:
// an `ok:false` provider carries ONLY id+ok; `p`/`s` may be int or float.
//
// Schema versions: v1 = {id,ok,p?,pr?,s?,sr?}. v2 is a strict SUPERSET — same
// fields plus an optional `cost` object (Claude and Pi providers this build). A v2 parser
// reads v1 unchanged (no cost => has_cost=false); both v1 and v2 are accepted.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STATS_MAX_PROVIDERS 12
#define STATS_ID_MAX        16
#define STATS_TXT_MAX       40
#define STATS_TS_MAX        28
#define STATS_HIST_MAX      31        // per-day cost points (cache holds ~20-31 days)
#define STATS_PCT_HIST_MAX  24        // 24h session usage-% sparkline points

typedef struct {
    char  id[STATS_ID_MAX];
    bool  ok;
    bool  has_p;  float p;            // primary used %
    char  pr[STATS_TXT_MAX];          // primary reset hint ("" if absent)
    bool  has_s;  float s;            // secondary used %
    char  sr[STATS_TXT_MAX];
    bool  has_t;  float t;            // tertiary used %
    char  tr[STATS_TXT_MAX];          // tertiary reset hint ("" if absent)

    // v2 cost-card-capable fields — present when publisher merged a generic
    // `cost` object (Claude/OpenRouter) or the Pi provider's sibling `pi` block.
    // For Pi: cost_today_c/cost_month_c alias max daily spend (ps), tok_today/tok_month alias
    // max daily tokens (pt) — Pi has no true today/today semantics. has_cost indicates presence.
    // Money is integer CENTS: LVGL's sprintf is compiled without float support, so the UI formats
    // with integer math. Token counts reach billions over 30 days -> 64-bit.
    bool     has_cost;
    int32_t  cost_today_c;            // spend today, cents
    int32_t  cost_month_c;            // spend trailing 30 days, cents
    int64_t  tok_today;               // tokens today
    int64_t  tok_month;               // tokens trailing 30 days
    int32_t  extra_used_c;            // extra-usage overage used, cents
    int32_t  extra_limit_c;           // extra-usage overage limit, cents
    int32_t  cost_week_c;             // spend this week, cents (OpenRouter keyUsageWeekly)
    int32_t  credits_remaining_c;     // account balance remaining, cents (OpenRouter balance)
    int32_t  credits_limit_c;         // total account credits, cents (OpenRouter totalCredits)
    int      hist_n;                  // valid entries in hist[] (0 => no chart)
    int32_t  hist[STATS_HIST_MAX];    // per-day spend, cents, oldest -> newest

    // v2 optional `ph`: ~24h SESSION usage-% history (0..100), oldest->newest.
    // Usage %, NOT cost — feeds the Usage-Limits card sparkline. Additive
    // within v2 (absent => pct_hist_n=0); not tied to has_cost.
    int      pct_hist_n;
    uint8_t  pct_hist[STATS_PCT_HIST_MAX];
} stats_provider_t;

typedef struct {
    int   v;
    char  ts[STATS_TS_MAX];           // publisher ISO-8601 timestamp
    int   n;                          // number of providers populated
    stats_provider_t p[STATS_MAX_PROVIDERS];
} stats_t;

typedef enum {
    STATS_PARSE_OK = 0,
    STATS_PARSE_NO_DATA,   // key absent / result null  -> show "waiting for publisher"
    STATS_PARSE_BAD,       // malformed JSON            -> show parse error
} stats_parse_t;

stats_parse_t stats_model_parse(const char *body, stats_t *out);
