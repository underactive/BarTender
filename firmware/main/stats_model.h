// firmware/main/stats_model.h
//
// Two-step parse of the Upstash response into a flat struct.
//   body  = {"result":"<escaped-json>"}            (Upstash envelope)
//   inner = {v,ts,providers:[{id,ok,p?,pr?,s?,sr?}]}  (our payload)
// Field optionality matches the live contract verified against real bytes:
// an `ok:false` provider carries ONLY id+ok; `p`/`s` may be int or float.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define STATS_MAX_PROVIDERS 12
#define STATS_ID_MAX        16
#define STATS_TXT_MAX       40
#define STATS_TS_MAX        28

typedef struct {
    char  id[STATS_ID_MAX];
    bool  ok;
    bool  has_p;  float p;            // primary used %
    char  pr[STATS_TXT_MAX];          // primary reset hint ("" if absent)
    bool  has_s;  float s;            // secondary used %
    char  sr[STATS_TXT_MAX];
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
