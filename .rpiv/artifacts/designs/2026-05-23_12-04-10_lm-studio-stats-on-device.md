---
date: 2026-05-23T12:04:10-0700
author: Eric Sison
commit: 5da5c88
branch: master
repository: bartender
topic: "LM Studio stats on device"
tags: [design, firmware, scripts, lm-studio, pipeline]
status: ready
last_updated: 2026-05-23T12:04:10-0700
last_updated_by: Eric Sison
---

# Design: LM Studio Stats Provider

## Summary
Add a new `lmstudio` provider to the BarTender pipeline that surfaces local LM Studio inference metrics — requests, tokens, cache utilization, model popularity, and 7-day daily usage — through a dedicated `lm` sub-object in the payload. The firmware gains four custom pages (TODAY, STATS 1, STATS 2, STATS 3) with dual progress bars on the summary row and purple accent (`0x7C3AED`). Implementation follows the Pi Agent provider pattern: host script rewrite as zsh+Python, publisher merge stage with timeout guard, dedicated `lm_*` fields on `stats_provider_t`, and `is_lmstudio` branching in `render_card()`.

## Requirements
- Parse `~/.lmstudio/server-logs/` logs to extract daily requests, tokens, cache utilization, and model usage
- Publish LM Studio metrics to Upstash as a new `lmstudio` provider entry with an `lm` sub-object
- Show two progress bars on the summary page (Tokens bar + Requests bar) under the entry, using the LM Studio purple accent
- Provide 4 custom detail pages: TODAY (hero tokens, hero requests, 30-day bar chart, 30-day max), STATS 1 (tokens %, requests %), STATS 2 (top-10 models table), STATS 3 (7-day daily usage table)
- Store a rolling 30-day history of daily requests and tokens for max-value comparison bar charts
- Only surface daily-aggregate stats — no per-request data, no raw prompt content
- Cache % and Cache Hit % gracefully show 'N/A' when the backend doesn't report cache_wrapper data

## Current State Analysis
The existing `scripts/lmstudio-stats.sh` is a standalone Python3 script that parses log files and outputs a single-day snapshot. It does NOT follow the `pi-agent-stats.sh` pattern (zsh wrapper + inline Python + rolling history). The publisher (`codexbar-publish.sh`) has no LM Studio merge stage. The firmware has no `lm_*` fields, no `has_lm` flag, no LM Studio card types, and no `is_lmstudio` rendering branches.

### Key Discoveries
- `scripts/lmstudio-stats.sh:1` — current `#!/usr/bin/env python3` shebang, needs full rewrite to zsh wrapper pattern
- `scripts/lmstudio-stats.sh:41-77` — 7 regex patterns for log parsing (RE_REQUEST, RE_MODEL, RE_RELEASE, RE_CACHE, RE_CACHE_LOOKUP, RE_CACHE_FOUND, RE_PINNED)
- `scripts/pi-agent-stats.sh:31` — Python discovery pattern: `PY="${PYTHON3:-$(command -v python3)}"`
- `scripts/pi-agent-stats.sh:35-end` — inline Python heredoc pattern
- `scripts/codexbar-publish.sh:59` — PI_STATS variable declaration template
- `scripts/codexbar-publish.sh:340-385` — PI_MERGE_JXA heredoc (validation, field extraction, replace/prepend)
- `scripts/codexbar-publish.sh:440-466` — Pi merge block with timeout 30 guard
- `firmware/main/stats_model.h:13-46` — stats_provider_t struct (needs lm_* fields)
- `firmware/main/stats_model.c:95-112` — pi sub-object parse (template for lm parse)
- `firmware/main/stats_model.c:33` — memset auto-zeroes new fields
- `firmware/main/stats_model.c:65` — version guard accepts v1/v2 only
- `firmware/main/ui.c:69` — card_kind_t enum (needs +2 entries)
- `firmware/main/ui.c:1086` — is_pi flag in render_card() (template for is_lmstudio)
- `firmware/main/ui.c:1621-1625` — NAV_PAGE tap handler (needs 4-card wrap for lmstudio)
- `firmware/main/ui.c:205-208` — provider_card_available() (needs LM card type handling)
- `firmware/main/ui.c:218-234` — saver_candidate_at() (needs LM card branching)
- `firmware/main/ui.c:908-918` — hide_cards() (needs to hide LM card panels)
- `firmware/main/provider_colors.h:39` — `"lmstudio": 0x7C3AED` already registered
- `firmware/main/provider_icons.c:1217` — `{"lmstudio", &ic_lmstudio}` already registered

### Constraints
- v2 payload superset: `lm` sub-object added within v2, no version bump
- Dedicated `lm_*` fields on `stats_provider_t` (not cost-shaped slot reuse)
- Dual history arrays: `lm_hr[]` (int32, 31) and `lm_ht[]` (int64, 31)
- Summary bars: Token % in primary bar, requests % in weekly bar slot
- Card navigation: 4-card wrap-around for LM Studio only
- Arrays of objects for models (`{id, rq}`) and week (`{d, rq, tk, cp, ch}`)

## Scope
### Building
- Host script rewrite (lmstudio-stats.sh): zsh wrapper + inline Python + rolling history
- Publisher merge (codexbar-publish.sh): LM_STATS var, LM_MERGE_JXA, cmd_once block
- Payload schema (schema.json): `lm` sub-object
- Firmware data model (stats_model.h/c): lm_* fields, lm parse branch
- Firmware summary UI (ui.c): dual bars, card_kind_t +2, navigation, screensaver
- Firmware card pages (ui.c): TODAY, STATS 1, STATS 2, STATS 3

### Not Building
- Cost/spend tracking for LM Studio (it's free/local)
- History storage on device (host-side only)
- Real-time log watching (parsed once per publish cycle)
- Per-model drill-down pages
- Device configuration UI for LM Studio settings

## Decisions
### Data model: Dedicated `lm_*` fields
**Evidence**: Research artifact confirms Pi's cost-shaped slot reuse doesn't fit LM Studio's dual-history needs. Dedicated fields avoid semantic mismatch.
**Decision**: Add `lm_req_today` (int32), `lm_tok_today` (int64), `lm_req_month_max` (int32), `lm_tok_month_max` (int64), `lm_cache_pct` (float), `lm_cache_hit_pct` (float), `lm_hr[]` (int32, 31), `lm_ht[]` (int64, 31), `lm_models_n` + `lm_models_id[][]` + `lm_models_req[]`, `lm_week_n` + `lm_week_*` arrays. Plus `has_lm` flag.

### Schema: `lm` sub-object within v2
**Evidence**: Pi was added within v2 in `f58693d`. The research artifact confirms: "Add within v2 (matching how pi was added)."
**Decision**: Declare optional `lm` sub-object in v2 schema. Fields: `rq`, `tk`, `mxr`, `mxt`, `cp`, `ch`, `hr[]`, `ht[]`, `models[{id,rq}]`, `week[{d,rq,tk,cp,ch}]`.

### Summary bars: Reuse weekly bar slot
**Developer Decision**: Token % in primary bar (`row_bar`), requests % in weekly bar (`row_bar_w`). LM Studio has no `has_s`, freeing the weekly bar slot.

### Card navigation: 4-page wrap-around
**Evidence**: Discover artifact chose "extend existing card system" with TODAY=COST slot, STATS 1=LIMITS slot, +2 new enums.
**Decision**: `CARD_COST` → TODAY hero page, `CARD_LIMITS` → STATS 1 percentage page, add `CARD_LM_STATS_2` (models table), `CARD_LM_STATS_3` (7-day table). Tap cycles TODAY→STATS 1→STATS 2→STATS 3→TODAY.

### Models data: Array of objects
**Developer Decision**: `models: [{id: "model-name", rq: 42}, ...]` for top-10 models table.

### Week table: Array of objects
**Developer Decision**: `week: [{d: "05-16", rq: 5, tk: 1200, cp: 45.2, ch: 78.3}, ...]` for 7-day daily table.

### Publisher merge: PI_MERGE_JXA pattern
**Evidence**: Pi merge follows a validated pattern (id/ok check, field extraction, replace/prepend, write back) with timeout 30 guard. Precedent `4613728` added the guard 15 min post-merge.
**Decision**: Copy PI_MERGE_JXA → LM_MERGE_JXA with `lmstudio` id check and `lm` field extraction. Same timeout guard in cmd_once().

### Icon/color: Already registered
**Evidence**: `provider_colors.h:39` has `"lmstudio": 0x7C3AED`. `provider_icons.c:1217` has `{"lmstudio", &ic_lmstudio}`.
**Decision**: No changes to icon or color files.

## Architecture

### `firmware/main/stats_model.h` — MODIFY
```c
// Add after pct_hist (line ~52):
// LM Studio dedicated fields (no cost-spend, dual histories)
bool     has_lm;
int32_t  lm_req_today;
int64_t  lm_tok_today;
int32_t  lm_req_month_max;
int64_t  lm_tok_month_max;
float    lm_cache_pct;
float    lm_cache_hit_pct;
int      lm_hr_n;
int32_t  lm_hr[STATS_HIST_MAX];      // daily requests, oldest->newest
int      lm_ht_n;
int64_t  lm_ht[STATS_HIST_MAX];      // daily tokens, oldest->newest

// Top-10 models
#define LM_MODELS_MAX 10
int      lm_models_n;
char     lm_models_id[LM_MODELS_MAX][STATS_ID_MAX];
int32_t  lm_models_req[LM_MODELS_MAX];

// 7-day daily table
#define LM_WEEK_MAX  7
int      lm_week_n;
char     lm_week_d[LM_WEEK_MAX][6];    // "MM-DD"
int32_t  lm_week_rq[LM_WEEK_MAX];
int64_t  lm_week_tk[LM_WEEK_MAX];
float    lm_week_cp[LM_WEEK_MAX];
float    lm_week_ch[LM_WEEK_MAX];
```

### `firmware/main/stats_model.c` — MODIFY
```c
// Insert after the pi block (after line 165), before the ph block:
// v2 optional `lm` block: LM Studio publishes requests/tokens/cache/model data.
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
    // hr[] — daily request history
    const cJSON *hr = cJSON_GetObjectItemCaseSensitive(lm, "hr");
    if (cJSON_IsArray(hr)) {
        const cJSON *hv;
        cJSON_ArrayForEach(hv, hr) {
            if (p->lm_hr_n >= STATS_HIST_MAX) break;
            if (cJSON_IsNumber(hv)) { p->lm_hr[p->lm_hr_n++] = i32_clamp(hv->valuedouble); any_lm = true; }
        }
    }
    // ht[] — daily token history
    const cJSON *ht = cJSON_GetObjectItemCaseSensitive(lm, "ht");
    if (cJSON_IsArray(ht)) {
        const cJSON *hv;
        cJSON_ArrayForEach(hv, ht) {
            if (p->lm_ht_n >= STATS_HIST_MAX) break;
            if (cJSON_IsNumber(hv)) { p->lm_ht[p->lm_ht_n++] = i64_clamp(hv->valuedouble); any_lm = true; }
        }
    }
    // models[] — top-10 models
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
    // week[] — 7-day daily table
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
```

### `docs/generated/codexbar-payload.schema.json` — MODIFY
```json
"lm": {
  "type": "object",
  "description": "v2 optional, LM Studio provider only. Local inference metrics from ~/.lmstudio/server-logs/. Present only on provider id `lmstudio`.",
  "required": ["rq", "tk", "mxr", "mxt", "hr", "ht"],
  "properties": {
    "rq":  { "type": "integer", "description": "Requests today." },
    "tk":  { "type": "integer", "description": "Tokens today." },
    "mxr": { "type": "integer", "description": "30-day max daily requests." },
    "mxt": { "type": "integer", "description": "30-day max daily tokens." },
    "cp":  { "type": "number", "description": "Cache % (KV cache MiB / limit MiB), 0-100." },
    "ch":  { "type": "number", "description": "Cache hit % (avg cached/total tokens), 0-100." },
    "hr":  { "type": "array", "items": { "type": "integer" }, "maxItems": 31, "description": "Daily requests history, oldest->newest." },
    "ht":  { "type": "array", "items": { "type": "integer" }, "maxItems": 31, "description": "Daily tokens history, oldest->newest." },
    "models": {
      "type": "array", "maxItems": 10,
      "items": {
        "type": "object", "required": ["id", "rq"],
        "properties": {
          "id": { "type": "string", "description": "Model identifier from LM Studio request body." },
          "rq": { "type": "integer", "description": "Requests using this model today." }
        },
        "additionalProperties": false
      },
      "description": "Top-10 models by request count today."
    },
    "week": {
      "type": "array", "maxItems": 7,
      "items": {
        "type": "object", "required": ["d", "rq", "tk"],
        "properties": {
          "d":  { "type": "string", "description": "Date MM-DD." },
          "rq": { "type": "integer", "description": "Requests on this day." },
          "tk": { "type": "integer", "description": "Tokens on this day." },
          "cp": { "type": "number", "description": "Cache % on this day, 0-100." },
          "ch": { "type": "number", "description": "Cache hit % on this day, 0-100." }
        },
        "additionalProperties": false
      },
      "description": "7-day daily usage table."
    }
  },
  "additionalProperties": false
}
```

### `scripts/lmstudio-stats.sh` — REWRITE
Full rewrite as zsh wrapper with inline Python heredoc. See Slice 2 for implementation.

### `scripts/codexbar-publish.sh` — MODIFY
Add `LM_STATS` variable, `LM_MERGE_JXA` heredoc, cmd_once merge block. See Slice 3 for implementation.

### `firmware/main/ui.c` — MODIFY
card_kind_t enum, is_lmstudio flag, build_widgets additions, render_card branches, summary rendering, hide_cards, nav cycling, provider_card_available, saver_candidate_at, update_provider_activity. See Slices 4-5 for implementation.

## Slices

### Slice 1: Data Contract — types, parse, schema

**Files**: `firmware/main/stats_model.h`, `firmware/main/stats_model.c`, `docs/generated/codexbar-payload.schema.json`

#### Automated Verification:
- [ ] Type checking passes: `cd firmware && idf.py build`
- [ ] `stats_provider_t` has all `lm_*` fields and `has_lm`
- [ ] memset auto-zeroes all new fields (confirmed by code pattern at stats_model.c:33)
- [ ] Payload schema validates: `python3 -m json.tool docs/generated/codexbar-payload.schema.json`

#### Manual Verification:
- [ ] `stats_model_parse()` successfully parses a test `lm` sub-object
- [ ] Version guard still accepts v1 and v2; rejects others
- [ ] Parse correctly skips `lm` for non-lmstudio providers
- [ ] Cache % and Cache Hit % fields default gracefully when absent

### Slice 2: Producer — host script rewrite
**Files**: `scripts/lmstudio-stats.sh`

#### Automated Verification:
- [ ] Script runs without error: `./scripts/lmstudio-stats.sh`
- [ ] Output is valid JSON: `./scripts/lmstudio-stats.sh | python3 -m json.tool`
- [ ] Output has expected shape: `{"id":"lmstudio","ok":true,"p":...,"s":...,"lm":{...}}`
- [ ] Env overrides work: `LMSTUDIO_LOG_DIR` and `LMSTUDIO_HISTORY` accepted
- [ ] Rolling history file is created at `~/.config/codexbar-toy/lmstudio-history.json`
- [ ] Exits non-zero when no logs found (fail-soft)
- [ ] Cache fields (`cp`, `ch`) omitted from output when no cache data exists

#### Manual Verification:
- [ ] Runs against real `~/.lmstudio/server-logs/` and produces meaningful output
- [ ] Rolling history persists across runs (pruned to 31 days)

### Slice 3: Publisher — merge integration

**Files**: `scripts/codexbar-publish.sh`

#### Automated Verification:
- [ ] Shell syntax: `bash -n scripts/codexbar-publish.sh`
- [ ] Environment variables (`LM_STATS`, `CBPUB_LM_STATS`, `CBPUB_LM_JSON`) consistent
- [ ] JXA heredoc parses: `osascript -l JavaScript -e "$(awk '/^LM_MERGE_JXA/' RS= scripts/codexbar-publish.sh)"`

#### Manual Verification:
- [ ] `codexbar-publish.sh --once` successfully merges LM Studio data (when LM Studio server is running locally)
- [ ] On `lmstudio-stats.sh` failure/timeout, publisher logs skip and continues
- [ ] On malformed helper output, `LM_MERGE_JXA` exits non-zero and publisher logs skip
- [ ] LM Studio provider appears after Pi, before Claude in the final payload

### Slice 4: Summary UI — dual bars, navigation wiring

**Files**: `firmware/main/ui.c`

#### Automated Verification:
- [ ] Type checking passes: `cd firmware && idf.py build`
- [ ] `card_kind_t` enum has 4 entries: `CARD_COST`, `CARD_LIMITS`, `CARD_LM_STATS_2`, `CARD_LM_STATS_3`
- [ ] `is_lmstudio` flag correctly set in `render_card()` based on `p->id`
- [ ] Summary row shows `lmstudio` with dual bars (token % in row_bar, request % in row_bar_w)
- [ ] Weekly bar (`row_bar_w`) shows for `lmstudio` provider
- [ ] `is_hidden_provider()` does NOT include `lmstudio`
- [ ] Tap on lmstudio summary row opens TODAY page (`CARD_COST`)
- [ ] Screensaver cycles through LM Studio card types (4-card cycle in saver_advance_locked)
- [ ] Provider activity tracking includes LM Studio (provider_metric_sig hashes lm_* fields)
- [ ] provider_card_available() returns true for LM card types

#### Manual Verification:
- [ ] Summary list scroll works with all providers including LM Studio
- [ ] Purple accent color (`#7C3AED`) appears on LM Studio bars and header
- [ ] "off" text displays when LM Studio has no data (`ok:false`)
- [ ] Screensaver enters and cycles through LM Studio cards correctly
- [ ] Tap on LM Studio row opens TODAY page first

### Slice 5: Card Pages — 4 new LM Studio cards
**Files**: `firmware/main/ui.c`

#### Automated Verification:
- [ ] Type checking passes: `cd firmware && idf.py build`
- [ ] All new card widgets (`lm2_card`, `lm3_card`, `lm2_hdr`, `lm2_logo`, `lm2_table`, `lm3_hdr`, `lm3_logo`, `lm3_table`) are created in `build_widgets()`
- [ ] `render_card()` handles `CARD_LM_STATS_2` (models table) and `CARD_LM_STATS_3` (week table) cases
- [ ] `hide_cards()` hides `lm2_card` and `lm3_card`
- [ ] `provider_card_available()` returns `true` for LM card types when `has_lm`
- [ ] `saver_candidate_at()` selects initial card `CARD_COST` for LM Studio
- [ ] Tap cycles through 4 cards: TODAY → STATS 1 → MODELS → WEEK → TODAY
- [ ] LM Studio TODAY bypasses `!p->has_cost` guard via `!p->has_cost && !p->has_lm`
- [ ] `int64_t lm_ht[]` properly converted to int32_t for chart renderer

#### Manual Verification:
- [ ] TODAY page shows: hero token count (lemonmilk_48), smaller hero request count, 30-day token bar chart, "30 DAY MAX: X Toks • Y Reqs" at bottom
- [ ] STATS 1 page shows: TOKENS hero % (session style), REQUESTS sub-hero % (weekly style)
- [ ] MODELS page shows: top-10 models table (Rank, Model name, Request count) using LVGL table widget
- [ ] WEEK page shows: 7-day daily table (Date, Req, Toks, Cp%, Hit%) using LVGL table widget
- [ ] Cache % and Cache Hit % show 'N/A' when data unavailable
- [ ] Card transitions are smooth (no missing frames)
- [ ] Cards render correctly in both landscape (W=320) and portrait (W=240) orientations
- [ ] Table column widths fit within screen width

## Desired End State
When the pipeline is complete, a user with LM Studio running locally sees:
- A new `lmstudio` row on the device summary, with a purple token bar and a thinner requests bar below it
- Tapping opens TODAY page showing: "42.3K tokens today" (hero), "15 requests" (smaller), a 30-day purple bar chart, and "30 DAY MAX: 85.1K Toks • 120 Reqs" at the bottom
- Second tap shows STATS 1 with "45.3%" (tokens vs 30-day max) and "60.0%" (requests vs 30-day max)
- Third tap shows STATS 2 with a model table: "#1 llama-3.2-3b (15 req), #2 qwen-2.5-7b (10 req)..."
- Fourth tap shows STATS 3 with a 7-day table: "05-16 | 5 req | 1.2K tok | 45% | 78%"
- Fifth tap wraps back to TODAY

## File Map
- `firmware/main/stats_model.h`  # MODIFY — add lm_* fields and has_lm flag
- `firmware/main/stats_model.c`  # MODIFY — add lm sub-object parse branch
- `docs/generated/codexbar-payload.schema.json`  # MODIFY — add lm sub-object schema
- `scripts/lmstudio-stats.sh`  # REWRITE — zsh wrapper + inline Python + rolling history
- `scripts/codexbar-publish.sh`  # MODIFY — add LM_STATS var, LM_MERGE_JXA, cmd_once block
- `firmware/main/ui.c`  # MODIFY — cards, nav, summary bars, screensaver

## Ordering Constraints
- Slice 1 (Data Contract) must come first — defines types and schema
- Slice 2 (Producer) depends on Slice 1 schema
- Slice 3 (Publisher) depends on Slice 2 (runs lmstudio-stats.sh)
- Slice 4 (Summary UI) depends on Slice 1 (uses data model)
- Slice 5 (Card Pages) depends on Slice 4 (needs card_kind_t enums + nav context)

## Verification Notes
- Publisher merge fail-soft is critical: timeout 30 guard, exit-code checking, malformed JSON handling. Precedent `4613728` added timeout guard 15 min post-merge for Pi — include from the start for LM.
- Card layout for new provider always needs visual fixup: OpenRouter needed layout fix 54 min later (`78c9cfe`), sub-1% formatting bug fixed same day (`93bfd4b`). LM Studio's 4 card pages need visual review.
- Font glyph gaps are recurring: U+002D hyphen fix needed for OpenRouter. LM Studio tables use numbers, dates, % signs, dash chars — verify LEMONMILK font range.
- No-cost provider must skip cost placeholders: LM Studio TODAY page must NOT show cost fields. The `has_balance` pattern is the right model (inverted).
- Provider activity detection must include LM Studio: `update_provider_activity_locked()` filter `!p->has_cost && !provider_has_limits_card(p)` will exclude LM Studio — must add `|| p->has_lm`.
- Screensaver must handle 4-card LM cycle: `saver_advance_locked()` currently cycles CARD_COST→CARD_LIMITS. Must extend for LM's 4-card wrap.

## Performance Considerations
- Host script must complete within 10s (parsing up to ~50 log files per day, each up to ~10MB)
- Rolling history file is a single JSON file, read/updated every 5 min publish cycle
- Firmware: 4 new card pages must render within existing 5ms tick cycle
- New `stats_provider_t` grows by ~420 bytes, total ~2.3 KB (still fits in 4KB fetch buffer)
- LM Studio card widgets are created once in `build_widgets()` and shown/hidden — no dynamic allocation

## Migration Notes
Not applicable — no existing data schema to migrate. LM Studio logs are ephemeral (rotated by LM Studio); the rolling history file is created fresh on first run.

## Pattern References
- `scripts/pi-agent-stats.sh` — full pattern for host script (zsh wrapper + inline Python + env overrides + fail-soft exit codes)
- `scripts/codexbar-publish.sh:340-385` — PI_MERGE_JXA heredoc (validation pattern for LM_MERGE_JXA)
- `scripts/codexbar-publish.sh:440-466` — Pi merge block in cmd_once() (timeout guard template)
- `firmware/main/stats_model.c:95-112` — pi sub-object parse (template for lm parse block)
- `firmware/main/ui.c:1086` — is_pi flag (template for is_lmstudio flag)
- `firmware/main/ui.c:1621-1625` — current nav toggle (base for 4-card wrap)
- `docs/generated/codexbar-payload.schema.json:53-68` — pi schema (template for lm schema)

## Developer Context
**Q (`ui.c:1457-1459`): Summary weekly bar is hardcoded to claude/codex. LM Studio needs dual bars. How should requests bar display?**
**A**: Reuse the existing weekly bar slot (`row_bar_w`) for the Requests bar. LM Studio has no weekly/secondary pct, so the slot is available.

**Q: How should top-10 models data be structured in payload?**
**A**: Array of objects: `models: [{id: "model-name", rq: 42}, ...]`.

**Q: How should 7-day daily table data be structured in payload?**
**A**: Array of objects: `week: [{d: "05-16", rq: 5, tk: 1200, cp: 45.2, ch: 78.3}, ...]`.

## References
- `.rpiv/artifacts/research/2026-05-23_11-34-39_lm-studio-stats-on-device.md` — Comprehensive research with Code References, Integration Points, Precedents & Lessons
- `.rpiv/artifacts/discover/2026-05-23_10-41-20_lm-studio-stats.md` — Feature Requirements Document with all 11 decisions
- `.rpiv/artifacts/research/2026-05-22_13-48-57_pi-agent-stats.md` — Pi Agent research (established pattern for provider sub-object)

## Design History
- Slice 1: Data Contract — approved as generated
- Slice 2: Producer — approved as generated
- Slice 3: Publisher — approved as generated
- Slice 4: Summary UI — approved as generated
- Slice 5: Card Pages — approved as generated