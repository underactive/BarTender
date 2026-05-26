---
date: 2026-05-25T17:22:38-0700
author: Eric Sison
commit: 5da5c88
branch: master
repository: bartender
topic: "LM Studio stats on device"
tags: [plan, firmware, scripts, lm-studio, pipeline]
status: ready
parent: ".rpiv/artifacts/designs/2026-05-23_12-04-10_lm-studio-stats-on-device.md"
last_updated: 2026-05-25T17:22:38-0700
last_updated_by: Eric Sison
---

# LM Studio Stats Provider Implementation Plan

## Overview

Add a new `lmstudio` provider to the BarTender pipeline that surfaces local LM Studio inference metrics — requests, tokens, cache utilization, model popularity, and 7-day daily usage — through a dedicated `lm` sub-object in the payload. The firmware gains four custom pages (TODAY, STATS 1, STATS 2, STATS 3) with dual progress bars on the summary row and purple accent (`0x7C3AED`). Implementation follows the Pi Agent provider pattern end-to-end.

Design: `.rpiv/artifacts/designs/2026-05-23_12-04-10_lm-studio-stats-on-device.md`

## Desired End State

When the pipeline is complete, a user with LM Studio running locally sees:
- A new `lmstudio` row on the device summary, with a purple token bar and a thinner requests bar below it
- Tapping opens TODAY page showing: "42.3K tokens today" (hero), "15 requests" (smaller), a 30-day purple bar chart, and "30 DAY MAX: 85.1K Toks • 120 Reqs" at the bottom
- Second tap shows STATS 1 with "45.3%" (tokens vs 30-day max) and "60.0%" (requests vs 30-day max)
- Third tap shows STATS 2 with a model table: "#1 llama-3.2-3b (15 req), #2 qwen-2.5-7b (10 req)..."
- Fourth tap shows STATS 3 with a 7-day table: "05-16 | 5 req | 1.2K tok | 45% | 78%"
- Fifth tap wraps back to TODAY

## What We're NOT Doing
- Cost/spend tracking for LM Studio (it's free/local)
- History storage on device (host-side only)
- Real-time log watching (parsed once per publish cycle)
- Per-model drill-down pages
- Device configuration UI for LM Studio settings

## Phase 1: Data Contract

### Overview
Define the data model contract: add dedicated `lm_*` fields to `stats_provider_t`, implement the `lm` sub-object parse branch in `stats_model.c`, and declare the `lm` sub-object schema in `codexbar-payload.schema.json`. This phase depends on nothing — it is the foundation.

### Changes Required:

#### 1. Firmware Data Model Header
**File**: `firmware/main/stats_model.h`
**Changes**: Add `has_lm`, `lm_req_today`, `lm_tok_today`, `lm_req_month_max`, `lm_tok_month_max`, `lm_cache_pct`, `lm_cache_hit_pct`, `lm_hr[]`, `lm_ht[]`, `lm_models_*`, `lm_week_*` fields to `stats_provider_t`. Add `LM_MODELS_MAX=10` and `LM_WEEK_MAX=7` macros.

```c
    // v2 optional `lm` block: LM Studio publishes local inference metrics
    // (requests, tokens, cache %, model breakdown, 7-day table) without any
    // cost/spend data. Dedicated fields (not cost-shaped slot reuse) because
    // LM Studio has dual parallel histories (requests + tokens) that don't
    // match the single cost-history shape.
    bool     has_lm;
    int32_t  lm_req_today;          // requests today
    int64_t  lm_tok_today;          // tokens today
    int32_t  lm_req_month_max;      // 30-day max daily requests
    int64_t  lm_tok_month_max;      // 30-day max daily tokens
    float    lm_cache_pct;          // cache % (KV cache MiB / limit MiB), 0-100
    float    lm_cache_hit_pct;      // cache hit % (avg cached/total tokens), 0-100
    int      lm_hr_n;               // valid entries in lm_hr[]
    int32_t  lm_hr[STATS_HIST_MAX];  // daily requests, oldest -> newest
    int      lm_ht_n;               // valid entries in lm_ht[]
    int64_t  lm_ht[STATS_HIST_MAX];  // daily tokens, oldest -> newest

#define LM_MODELS_MAX 10
    int      lm_models_n;
    char     lm_models_id[LM_MODELS_MAX][STATS_ID_MAX];
    int32_t  lm_models_req[LM_MODELS_MAX];

#define LM_WEEK_MAX  7
    int      lm_week_n;
    char     lm_week_d[LM_WEEK_MAX][6]; // "MM-DD"
    int32_t  lm_week_rq[LM_WEEK_MAX];
    int64_t  lm_week_tk[LM_WEEK_MAX];
    float    lm_week_cp[LM_WEEK_MAX];
    float    lm_week_ch[LM_WEEK_MAX];
```

#### 2. Firmware Parser
**File**: `firmware/main/stats_model.c`
**Changes**: Add `lm` sub-object parse branch after the `pi` block (after `if (any_pi) p->has_cost = true;`) and before the `ph` block. Parses `rq`, `tk`, `mxr`, `mxt`, `cp`, `ch`, `hr[]`, `ht[]`, `models[{id,rq}]`, `week[{d,rq,tk,cp,ch}]` from the `lm` JSON object. Sets `p->has_lm = true` on any data found.

```c
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
```

#### 3. Payload Schema
**File**: `docs/generated/codexbar-payload.schema.json`
**Changes**: Add `lm` sub-object schema as a sibling of `cost`, `ph`, and `pi` in the provider items properties.

```json
          "lm": {
            "type": "object",
            "description": "v2 optional, LM Studio provider only. Local inference metrics from ~/.lmstudio/server-logs/. Present only on provider id `lmstudio`.",
            "required": ["rq", "tk", "mxr", "mxt"],
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

### Success Criteria:

#### Automated Verification:
- [x] Type checking passes: `cd firmware && idf.py build`
- [x] `stats_provider_t` has all `lm_*` fields and `has_lm`
- [x] memset auto-zeroes all new fields (confirmed by code pattern at stats_model.c:33)
- [x] Payload schema validates: `python3 -m json.tool docs/generated/codexbar-payload.schema.json`

#### Manual Verification:
- [x] `stats_model_parse()` successfully parses a test `lm` sub-object
- [x] Version guard still accepts v1 and v2; rejects others
- [x] Parse correctly skips `lm` for non-lmstudio providers
- [x] Cache % and Cache Hit % fields default gracefully when absent

---

## Phase 2: Producer

### Overview
Rewrite `scripts/lmstudio-stats.sh` as a zsh wrapper with inline Python heredoc following the `pi-agent-stats.sh` pattern. Adds rolling history persistence at `~/.config/codexbar-toy/lmstudio-history.json`, parses LM Studio server logs with 7 regex patterns, and emits compact JSON with the `lm` sub-object.

This phase depends on Phase 1 (must match the `lm` sub-object field names defined in the schema).

### Changes Required:

#### 1. Host Script Rewrite
**File**: `scripts/lmstudio-stats.sh`
**Changes**: Full rewrite. zsh shebang, env overrides (`LMSTUDIO_LOG_DIR`, `LMSTUDIO_HISTORY`, `PYTHON3`), inline Python heredoc. Parses `~/.lmstudio/server-logs/{YYYY-MM}/{YYYY-MM-DD.N.log}` files. Maintains rolling history at `~/.config/codexbar-toy/lmstudio-history.json`. Emits `{"id":"lmstudio","ok":true/false,"p":...,"s":...,"lm":{"rq","tk","mxr","mxt","cp?","ch?","hr[],"ht[],"models[{id,rq}],"week[{d,rq,tk,cp,ch}]"}}`. Exits 3 on no data (fail-soft).

```zsh
#!/bin/zsh
# lmstudio-stats.sh — reduce local LM Studio inference logs into the compact
# provider object merged by codexbar-publish.sh.
#
#   lmstudio-stats.sh             # emits one JSON provider object for id="lmstudio"
#   lmstudio-stats.sh --help
#
# Reads local LM Studio server logs only:
#   ~/.lmstudio/server-logs/{YYYY-MM}/{YYYY-MM-DD.N.log}   daily server logs
#   ~/.config/codexbar-toy/lmstudio-history.json            rolling 30-day history
#
# Output is privacy-reduced and contains no raw prompts, conversation context,
# response IDs, or API keys. Only daily-aggregated counts and percentages:
#   {"id":"lmstudio","ok":true,"p":45.3,"s":60.0,
#    "lm":{"rq":42,"tk":123456,"mxr":100,"mxt":500000,
#          "cp":45.2,"ch":78.3,"hr":[...],"ht":[...],
#          "models":[...],"week":[...]}}
#
# Field units:
#   p         tokens % vs 30-day max daily tokens
#   s         requests % vs 30-day max daily requests
#   lm.rq     requests today
#   lm.tk     tokens today
#   lm.mxr    30-day max daily requests
#   lm.mxt    30-day max daily tokens
#   lm.cp     cache % (KV cache MiB / limit MiB), 0-100
#   lm.ch     cache hit % (avg cached/total tokens), 0-100
#   lm.hr[]   daily requests history, oldest -> newest, up to 31 points
#   lm.ht[]   daily tokens history, oldest -> newest, up to 31 points
#   lm.models[]  top-10 models [{id,rq}]
#   lm.week[]    7-day daily table [{d,rq,tk,cp,ch}]
#
# Env overrides (testability hooks; default to real user paths):
#   LMSTUDIO_LOG_DIR     default: ~/.lmstudio/server-logs
#   LMSTUDIO_HISTORY     default: ~/.config/codexbar-toy/lmstudio-history.json
#   PYTHON3              Python interpreter (default: command -v python3)
#
# Fail-soft contract: exits non-zero when no usable local LM Studio usage
# exists; the publisher logs the skip and continues with the payload unchanged.
set -u

help() { awk 'NR>=2 && /^#/{sub(/^# ?/,"");print;next} NR>=2{exit}' "$0"; }
case "${1:-}" in
  "") ;;
  -h|--help) help; exit 0 ;;
  *) print -r -- "unknown argument: $1 (try --help)" >&2; exit 2 ;;
esac

PY="${PYTHON3:-$(command -v python3 2>/dev/null || true)}"
[[ -n "$PY" && -x "$PY" ]] || { print -r -- "lmstudio-stats: python3 not found" >&2; exit 2; }

"$PY" <<'PY'
import datetime as dt
import json
import os
import re
import sys
from pathlib import Path


def eprint(msg: str) -> None:
    print(f"lmstudio-stats: {msg}", file=sys.stderr)


# Environment overrides
home = Path.home()
log_dir = Path(os.environ.get("LMSTUDIO_LOG_DIR", home / ".lmstudio" / "server-logs")).expanduser()
hist_file = Path(os.environ.get("LMSTUDIO_HISTORY", home / ".config" / "codexbar-toy" / "lmstudio-history.json")).expanduser()

# Regex patterns (mirrored from original lmstudio-stats.sh)
RE_REQUEST      = re.compile(
    r'\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\].*Received request: POST to /v1/chat/completions'
)
RE_MODEL        = re.compile(r'"model":\s*"([^"]+)"')
RE_RELEASE      = re.compile(
    r'slot\s+release:\s+id\s+\d+\s+\|\s+task\s+\d+\s+\|\s+stop processing:\s+n_tokens\s*=\s*(\d+)'
)
RE_CACHE        = re.compile(
    r'cache state:\s+(\d+)\s+prompts?,\s+([\d.]+)\s+MiB\s+\(limits:\s+([\d.]+)\s+MiB'
)
RE_CACHE_HIT    = re.compile(r'selected slot by LCP similarity')
RE_CACHE_MISS   = re.compile(r'selected slot by LRU')
RE_CACHE_WRAPPER = re.compile(
    r'\[cache_wrapper\]\s+Prompt cache:\s+using\s+(\d+)\s*/\s*(\d+)\s+tokens from cache'
)


def parse_log(path: str, today: dt.date) -> dict:
    """Parse a single LM Studio server log file and return daily metrics."""
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        content = f.read()

    lines = content.split('\n')
    day_str = today.isoformat()

    requests = 0
    model_counts = {}
    total_slot_tokens = 0
    slot_release_count = 0
    latest_cache_pct = 0.0
    latest_cache_prompts = 0
    cache_hits = 0
    cache_misses = 0
    total_cached_tokens = 0
    total_cache_prompt_tokens = 0

    for line in lines:
        if RE_REQUEST.search(line):
            requests += 1

        m = RE_MODEL.search(line)
        if m and '/v1/chat/completions' not in line:
            model = m.group(1)
            model_counts[model] = model_counts.get(model, 0) + 1

        m = RE_RELEASE.search(line)
        if m:
            tokens = int(m.group(1))
            total_slot_tokens += tokens
            slot_release_count += 1

        m = RE_CACHE.search(line)
        if m:
            prompts = int(m.group(1))
            used_mib = float(m.group(2))
            limit_mib = float(m.group(3))
            latest_cache_pct = (used_mib / limit_mib * 100) if limit_mib > 0 else 0
            latest_cache_prompts = prompts

        if RE_CACHE_HIT.search(line):
            cache_hits += 1
        if RE_CACHE_MISS.search(line):
            cache_misses += 1

        m = RE_CACHE_WRAPPER.search(line)
        if m:
            cached = int(m.group(1))
            total_c = int(m.group(2))
            total_cached_tokens += cached
            total_cache_prompt_tokens += total_c

    # Compute cache hit % from cache_wrapper lines if available
    cache_hit_pct = 0.0
    if total_cache_prompt_tokens > 0:
        cache_hit_pct = (total_cached_tokens / total_cache_prompt_tokens) * 100.0
    elif cache_hits + cache_misses > 0:
        total_sel = cache_hits + cache_misses
        cache_hit_pct = (cache_hits / total_sel * 100.0) if total_sel > 0 else 0.0

    # Deduplicate model counts by request count
    total_model = sum(model_counts.values())
    if requests > 0 and total_model > requests:
        ratio = total_model / requests
        if ratio > 1:
            model_counts = {k: max(1, round(v / ratio)) for k, v in model_counts.items()}

    return {
        "date": day_str,
        "requests": requests,
        "tokens": total_slot_tokens,
        "cache_pct": round(latest_cache_pct, 1),
        "cache_hit_pct": round(cache_hit_pct, 1),
        "model_counts": model_counts,
    }


def parse_recent_logs(log_dir: Path, days: int = 31) -> dict:
    """Parse log files from the last N days into a date-keyed dict."""
    today = dt.date.today()
    results = {}

    if not log_dir.is_dir():
        eprint(f"log dir absent: {log_dir}")
        return results

    for year_dir in sorted(log_dir.iterdir()):
        if not year_dir.is_dir():
            continue
        for log_file in sorted(year_dir.iterdir()):
            if not log_file.name.endswith('.log'):
                continue
            try:
                parts = log_file.name.split('.')
                date_str = parts[0]
                fdate = dt.date.fromisoformat(date_str)
            except (ValueError, IndexError):
                continue
            age = (today - fdate).days
            if age < 0 or age >= days:
                continue
            try:
                stats = parse_log(str(log_file), fdate)
                date_key = fdate.isoformat()
                if date_key in results:
                    results[date_key]["requests"] += stats["requests"]
                    results[date_key]["tokens"] += stats["tokens"]
                    if stats["cache_pct"] > 0:
                        results[date_key]["cache_pct"] = stats["cache_pct"]
                    if stats["cache_hit_pct"] > 0:
                        results[date_key]["cache_hit_pct"] = stats["cache_hit_pct"]
                    for m, c in stats["model_counts"].items():
                        results[date_key]["model_counts"][m] = \
                            results[date_key]["model_counts"].get(m, 0) + c
                else:
                    results[date_key] = stats
            except Exception as e:
                eprint(f"error parsing {log_file}: {e}")
                continue

    return results


today = dt.date.today()
parsed = parse_recent_logs(log_dir, days=31)
today_str = today.isoformat()

today_data = parsed.get(today_str)
if today_data is None or (today_data["requests"] == 0 and today_data["tokens"] == 0):
    eprint(f"no usable data for {today_str}")
    today_data = {"date": today_str, "requests": 0, "tokens": 0,
                  "cache_pct": 0.0, "cache_hit_pct": 0.0, "model_counts": {}}

# Rolling history file
history = {}
if hist_file.is_file():
    try:
        history = json.loads(hist_file.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        eprint("history file corrupted, starting fresh")
        history = {}

# Merge today's data into history
if today_str not in history:
    history[today_str] = {
        "rq": today_data["requests"],
        "tk": today_data["tokens"],
        "cp": today_data["cache_pct"],
        "ch": today_data["cache_hit_pct"],
    }
else:
    existing = history[today_str]
    if existing.get("rq", 0) != today_data["requests"] or \
       existing.get("tk", 0) != today_data["tokens"]:
        history[today_str] = {
            "rq": today_data["requests"],
            "tk": today_data["tokens"],
            "cp": today_data["cache_pct"],
            "ch": today_data["cache_hit_pct"],
        }

# Prune to 31 days
sorted_dates = sorted(history.keys(), reverse=True)
keep = set(sorted_dates[:31])
history = {k: v for k, v in history.items() if k in keep}

hist_file.parent.mkdir(parents=True, exist_ok=True)
try:
    hist_file.write_text(json.dumps(history, indent=2), encoding="utf-8")
except OSError as e:
    eprint(f"could not write history: {e}")

# Compute daily arrays and max values from history (30-day window)
sorted_dates_asc = sorted(k for k in history.keys() if k <= today_str)
req_hist = []
tok_hist = []
max_req = 0
max_tok = 0
week_entries = []
week_dates = sorted([d for d in sorted_dates_asc if d >= (today - dt.timedelta(days=6)).isoformat()])

for d in sorted_dates_asc:
    entry = history[d]
    r = entry.get("rq", 0)
    t = entry.get("tk", 0)
    req_hist.append(r)
    tok_hist.append(t)
    if r > max_req:
        max_req = r
    if t > max_tok:
        max_tok = t
    if d in week_dates:
        week_entries.append({
            "d": d[-5:],
            "rq": r,
            "tk": t,
            "cp": entry.get("cp", 0.0),
            "ch": entry.get("ch", 0.0),
        })

tok_pct = 0.0
req_pct = 0.0
if max_tok > 0:
    tok_pct = round((today_data["tokens"] * 100.0) / max_tok, 1)
if max_req > 0:
    req_pct = round((today_data["requests"] * 100.0) / max_req, 1)
tok_pct = max(0.0, min(100.0, tok_pct))
req_pct = max(0.0, min(100.0, req_pct))

top_models = sorted(today_data["model_counts"].items(), key=lambda x: -x[1])[:10]
model_list = [{"id": m, "rq": c} for m, c in top_models]

any_data = today_data["requests"] > 0 or today_data["tokens"] > 0
has_history = any(v.get("rq", 0) > 0 or v.get("tk", 0) > 0 for v in history.values())
ok = any_data or has_history
if not ok:
    eprint("no usable LM Studio data")
    provider = {"id": "lmstudio", "ok": False}
    print(json.dumps(provider, separators=(",", ":")))
    sys.exit(3)

provider = {
    "id": "lmstudio",
    "ok": True,
    "p": tok_pct,
    "s": req_pct,
    "lm": {
        "rq": today_data["requests"],
        "tk": today_data["tokens"],
        "mxr": max_req,
        "mxt": max_tok,
        "hr": req_hist[-31:],
        "ht": tok_hist[-31:],
        "models": model_list,
        "week": week_entries[-7:],
    },
}

# Conditionally add cache fields (omit when 0 so firmware can show N/A)
if today_data["cache_pct"] > 0:
    provider["lm"]["cp"] = today_data["cache_pct"]
if today_data["cache_hit_pct"] > 0:
    provider["lm"]["ch"] = today_data["cache_hit_pct"]

print(json.dumps(provider, separators=(",", ":")))
eprint(
    f"today: {today_data['requests']}rq/{today_data['tokens']}tok "
    f"max: {max_req}rq/{max_tok}tok "
    f"hist: {len(req_hist)}d "
    f"models: {len(model_list)} "
    f"week: {len(week_entries)}d"
)
PY
```

### Success Criteria:

#### Automated Verification:
- [x] Script runs without error: `./scripts/lmstudio-stats.sh`
- [x] Output is valid JSON: `./scripts/lmstudio-stats.sh | python3 -m json.tool`
- [x] Output has expected shape: `{"id":"lmstudio","ok":true,"p":...,"s":...,"lm":{...}}`
- [x] Env overrides work: `LMSTUDIO_LOG_DIR` and `LMSTUDIO_HISTORY` accepted
- [x] Rolling history file is created at `~/.config/codexbar-toy/lmstudio-history.json`
- [x] Exits non-zero when no logs found (fail-soft)
- [x] Cache fields (`cp`, `ch`) omitted from output when no cache data exists

#### Manual Verification:
- [x] Runs against real `~/.lmstudio/server-logs/` and produces meaningful output
- [x] Rolling history persists across runs (pruned to 31 days)

---

## Phase 3: Publisher

### Overview
Integrate the LM Studio stats script into `codexbar-publish.sh`. Add `LM_STATS` variable, `LM_MERGE_JXA` heredoc (following `PI_MERGE_JXA` pattern exactly), and a timeout-30-guarded merge block in `cmd_once()`.

This phase depends on Phase 2 (runs `lmstudio-stats.sh`).

### Changes Required:

#### 1. Publisher Script
**File**: `scripts/codexbar-publish.sh`
**Changes**: (a) Add `LM_STATS` variable declaration after `PI_STATS`. (b) Add `LM_MERGE_JXA` heredoc after `PI_MERGE_JXA`. (c) Add cmd_once merge block after the Pi merge block, before `local tok`.

```zsh
# (a) Variable declaration (after PI_STATS at line 59):
LM_STATS="${CBPUB_LM_STATS:-$SELF_DIR/lmstudio-stats.sh}"

# (b) JXA heredoc (after PI_MERGE_JXA):
# Merge the reduced LM Studio provider object emitted by lmstudio-stats.sh into
# the provider array. Same fail-safe contract as PI_MERGE_JXA.
read -r -d '' LM_MERGE_JXA <<'EOF'
ObjC.import('Foundation'); ObjC.import('stdlib');
function env(k){ var v=$.NSProcessInfo.processInfo.environment.objectForKey(k);
  return (v && v.js!==undefined) ? String(v.js) : ''; }
function eprint(s){ $.NSFileHandle.fileHandleWithStandardError
  .writeData($.NSString.alloc.initWithUTF8String("lm-merge: "+s+"\n").dataUsingEncoding(4)); }
function rf(p){ try { var s=$.NSString.stringWithContentsOfFileEncodingError(p,4,null);
  return (s && s.js!==undefined) ? s.js : null; } catch(e){ return null; } }
function i32(v){ var n=Number(v); if(isNaN(n)) return null;
  if(n < -2147483648) n=-2147483648; if(n > 2147483647) n=2147483647;
  return Math.round(n); }
function i64(v){ var n=Number(v); return isNaN(n) ? null : Math.round(n); }
function num(v){ var n=Number(v); return isNaN(n) ? null : n; }
var jsonPath=env('CBPUB_JSON'), lmPath=env('CBPUB_LM_JSON');
if(!jsonPath || !lmPath){ eprint('missing CBPUB_JSON/CBPUB_LM_JSON'); $.exit(2); }
var pTxt=rf(jsonPath), lmTxt=rf(lmPath);
if(!pTxt || !lmTxt){ eprint('payload/helper unreadable'); $.exit(2); }
var pay, src;
try{ pay=JSON.parse(pTxt); }catch(e){ eprint('payload parse fail'); $.exit(2); }
try{ src=JSON.parse(lmTxt); }catch(e){ eprint('helper parse fail'); $.exit(3); }
if(!pay || !Array.isArray(pay.providers)){ eprint('payload shape'); $.exit(2); }
if(!src || src.id!=='lmstudio' || src.ok!==true || !src.lm || typeof src.lm!=='object' || Array.isArray(src.lm)){
  eprint('helper shape'); $.exit(3);
}
var rq=i32(src.lm.rq), tk=i64(src.lm.tk), mxr=i32(src.lm.mxr), mxt=i64(src.lm.mxt);
var p=num(src.p), s=num(src.s);
if(rq===null || tk===null || mxr===null || mxt===null){ eprint('helper lm fields'); $.exit(3); }
var dst={id:'lmstudio', ok:true, lm:{rq:rq, tk:tk, mxr:mxr, mxt:mxt}};
if(p!==null){ if(p<0)p=0; if(p>100)p=100; dst.p=Math.round(p*10)/10; }
if(s!==null){ if(s<0)s=0; if(s>100)s=100; dst.s=Math.round(s*10)/10; }
if(src.lm.cp!==undefined && src.lm.cp!==null){ var cp=num(src.lm.cp); if(cp!==null) dst.lm.cp=cp; }
if(src.lm.ch!==undefined && src.lm.ch!==null){ var ch=num(src.lm.ch); if(ch!==null) dst.lm.ch=ch; }
if(Array.isArray(src.lm.hr)){ var hr=[];
  for(var i=0;i<src.lm.hr.length && hr.length<31;i++){ var hv=i32(src.lm.hr[i]); if(hv!==null) hr.push(hv); }
  if(hr.length) dst.lm.hr=hr; }
if(Array.isArray(src.lm.ht)){ var ht=[];
  for(var i=0;i<src.lm.ht.length && ht.length<31;i++){ var hv=i64(src.lm.ht[i]); if(hv!==null) ht.push(hv); }
  if(ht.length) dst.lm.ht=ht; }
if(Array.isArray(src.lm.models)){ dst.lm.models=[];
  for(var i=0;i<Math.min(src.lm.models.length,10);i++){ var m=src.lm.models[i];
    if(m && typeof m.id==='string' && m.id && typeof m.rq==='number'){
      dst.lm.models.push({id:m.id, rq:i32(m.rq)}); } } }
if(Array.isArray(src.lm.week)){ dst.lm.week=[];
  for(var i=0;i<Math.min(src.lm.week.length,7);i++){ var w=src.lm.week[i];
    if(w && typeof w.d==='string' && w.d && typeof w.rq==='number'){
      var we={d:w.d, rq:i32(w.rq), tk:i64(w.tk)};
      if(typeof w.cp==='number') we.cp=num(w.cp);
      if(typeof w.ch==='number') we.ch=num(w.ch);
      dst.lm.week.push(we); } } }
var hadLm=false;
var next=[];
for(var i=0;i<pay.providers.length;i++){
  if(pay.providers[i] && pay.providers[i].id==='lmstudio') { hadLm=true; continue; }
  next.push(pay.providers[i]);
}
var piIdx=-1;
for(var i=0;i<next.length;i++){ if(next[i] && next[i].id==='pi'){ piIdx=i; break; } }
if(piIdx>=0) next.splice(piIdx+1,0,dst); else next.unshift(dst);
pay.providers=next;
var w=$.NSString.alloc.initWithUTF8String(JSON.stringify(pay))
  .writeToFileAtomicallyEncodingError(jsonPath,true,4,null);
if(!w){ eprint('payload writeback failed'); $.exit(2); }
eprint((hadLm?'replaced':'prepended')+' lmstudio provider: today='+rq+'rq/'+tk+'tok');
$.exit(0);
EOF

# (c) cmd_once merge block (after Pi block, before `local tok`):
  # Append/replace the LM Studio provider from local lmstudio-stats.sh.
  # Same fail-safe contract as Pi Agent: timeout guard, exit-code checking.
  local lm_json="$work/lm.json"
  local lm_rc=127
  if [[ -x "$LM_STATS" ]]; then
    if command -v timeout >/dev/null 2>&1; then
      timeout 30 "$LM_STATS" >"$lm_json" 2>>"$LOG"; lm_rc=$?
    elif command -v gtimeout >/dev/null 2>&1; then
      gtimeout 30 "$LM_STATS" >"$lm_json" 2>>"$LOG"; lm_rc=$?
    else
      "$LM_STATS" >"$lm_json" 2>>"$LOG"; lm_rc=$?
    fi
    if [[ $lm_rc -eq 0 ]]; then
      if CBPUB_JSON="$json" CBPUB_LM_JSON="$lm_json" osascript -l JavaScript -e "$LM_MERGE_JXA" 2>>"$LOG"; then
        bytes=$(wc -c <"$json" | tr -d ' ')
      else
        log "note: LM Studio merge skipped (malformed helper output) — publishing without LM Studio"
      fi
    elif [[ $lm_rc -eq 124 ]]; then
      log "note: LM Studio helper timed out after 30s — publishing without LM Studio"
    else
      log "note: LM Studio helper failed (exit code $lm_rc) — publishing without LM Studio"
    fi
  else
    log "note: LM Studio stats skipped (absent/unrecognized) — publishing without LM Studio"
  fi
```

### Success Criteria:

#### Automated Verification:
- [x] Shell syntax: `bash -n scripts/codexbar-publish.sh`
- [x] Environment variables (`LM_STATS`, `CBPUB_LM_STATS`, `CBPUB_LM_JSON`) consistent
- [x] JXA heredoc parses: `osascript -l JavaScript -e "$(awk '/^LM_MERGE_JXA/' RS= scripts/codexbar-publish.sh)"`

#### Manual Verification:
- [ ] `codexbar-publish.sh --once` successfully merges LM Studio data (when LM Studio server is running locally)
- [ ] On `lmstudio-stats.sh` failure/timeout, publisher logs skip and continues
- [ ] On malformed helper output, `LM_MERGE_JXA` exits non-zero and publisher logs skip
- [ ] LM Studio provider appears after Pi, before Claude in the final payload

---

## Phase 4: Summary UI — Dual Bars, Navigation Wiring

### Overview
Add LM Studio awareness to the firmware's summary and navigation system. Extend `card_kind_t` enum with `CARD_LM_STATS_2` and `CARD_LM_STATS_3`. Add `is_lmstudio` flag in `render_card()`. Wire summary dual bars (token % in `row_bar`, request % in `row_bar_w`). Update `provider_card_available()`, `update_provider_activity_locked()`, `provider_metric_sig()`, `saver_candidate_at()`, `saver_advance_locked()`, `hide_cards()`, `NAV_PAGE` tap handler, and summary row tap to handle LM Studio's 4-card cycle.

This phase depends on Phase 1 (uses `has_lm`, `lm_*` field names).

### Changes Required:

#### 1. Firmware UI
**File**: `firmware/main/ui.c`
**Changes**: 11 targeted changes spread across the file:

1. **`card_kind_t` enum** (line 69): Add `CARD_LM_STATS_2, CARD_LM_STATS_3`
2. **`is_lmstudio` flag** in `render_card()`: `bool is_lmstudio = (strcmp(p->id, "lmstudio") == 0);`
3. **`provider_card_available()`**: Switch statement: CARD_COST `p->has_cost || p->has_lm`, CARD_LIMITS `provider_has_limits_card(p) || p->has_lm`, CARD_LM_STATS_2/3 `p->has_lm`
4. **`update_provider_activity_locked()` filter**: `!p->has_cost && !provider_has_limits_card(p) && !p->has_lm`
5. **`provider_metric_sig()`**: Add LM Studio hashing block after pct_hist
6. **`saver_candidate_at()`**: `if (p->has_cost || p->has_lm) *card = CARD_COST`
7. **`saver_advance_locked()`**: LM Studio 4-card branch (COST→LIMITS→STATS_2→STATS_3→next)
8. **`hide_cards()`**: Also `lv_obj_add_flag(lm2_card, LV_OBJ_FLAG_HIDDEN)` and `lm3_card`
9. **`NAV_PAGE` tap handler**: LM Studio `(card_kind_t)(((int)st.nav_card + 1) % 4)`
10. **Summary row tap**: LM Studio always opens `CARD_COST`
11. **Weekly bar condition**: Add `strcmp(p->id, "lmstudio") == 0` to show weekly bar

```c
// 1. card_kind_t enum (line 69):
typedef enum { CARD_COST, CARD_LIMITS, CARD_LM_STATS_2, CARD_LM_STATS_3 } card_kind_t;

// 2. is_lmstudio flag in render_card() (after is_pi):
bool is_lmstudio = (strcmp(p->id, "lmstudio") == 0);

// 3. provider_card_available():
static bool provider_card_available(const stats_provider_t *p, card_kind_t card)
{
    switch (card) {
        case CARD_COST:         return p->has_cost || p->has_lm;
        case CARD_LIMITS:       return provider_has_limits_card(p) || p->has_lm;
        case CARD_LM_STATS_2:   return p->has_lm;
        case CARD_LM_STATS_3:   return p->has_lm;
    }
    return false;
}

// 4. update_provider_activity_locked filter (add || p->has_lm):
if (!p->has_cost && !provider_has_limits_card(p) && !p->has_lm) continue;

// 5. provider_metric_sig() — after pct_hist hashing, before return h:
h = hash_mix_u32(h, p->has_lm ? 1U : 0U);
if (p->has_lm) {
    h = hash_mix_u32(h, (uint32_t)p->lm_req_today);
    h = hash_mix_u32(h, (uint32_t)p->lm_tok_today);
    h = hash_mix_u32(h, (uint32_t)(p->lm_tok_today >> 32));
    h = hash_mix_u32(h, (uint32_t)p->lm_req_month_max);
    h = hash_mix_u32(h, (uint32_t)p->lm_tok_month_max);
    h = hash_mix_u32(h, (uint32_t)(p->lm_tok_month_max >> 32));
    h = hash_mix_u32(h, (uint32_t)p->lm_hr_n);
    for (int i = 0; i < p->lm_hr_n && i < STATS_HIST_MAX; i++)
        h = hash_mix_u32(h, (uint32_t)p->lm_hr[i]);
    h = hash_mix_u32(h, (uint32_t)p->lm_ht_n);
    for (int i = 0; i < p->lm_ht_n && i < STATS_HIST_MAX; i++)
        h = hash_mix_u32(h, (uint32_t)p->lm_ht[i]);
    h = hash_mix_u32(h, (uint32_t)p->lm_models_n);
    h = hash_mix_u32(h, (uint32_t)p->lm_week_n);
}

// 6. saver_candidate_at() initial card:
if (p->has_cost || p->has_lm) *card = CARD_COST;
else if (provider_has_limits_card(p)) *card = CARD_LIMITS;

// 7. saver_advance_locked() — LM 4-card branch before existing CARD_COST→CARD_LIMITS cycle:
if (pi >= 0 && strcmp(st.stats.p[pi].id, "lmstudio") == 0) {
    int next_card = ((int)st.saver_card + 1) % 4;
    if (next_card == 0 && st.saver_card == CARD_LM_STATS_3) {
        int pi = find_provider_id(st.saver_id);
        int start = pi >= 0 ? pi + 1 : 0;
        if (!saver_candidate_at(now, start, st.saver_next_id, sizeof st.saver_next_id, &st.saver_next_card)) {
            st.saver_next_dim_only = false;
            st.saver_next_show_summary = true;
        } else {
            st.saver_next_dim_only = false;
            st.saver_next_show_summary = false;
        }
        goto advance_done;
    }
    strlcpy(st.saver_next_id, st.saver_id, sizeof st.saver_next_id);
    st.saver_next_card = (card_kind_t)next_card;
    st.saver_next_dim_only = false;
    st.saver_next_show_summary = false;
} else if (pi >= 0 && st.saver_card == CARD_COST && provider_card_available(&st.stats.p[pi], CARD_LIMITS)) {
    // existing: Cycle Cost -> Limits on the same provider
    ...
}
// Add at the fade-start block: advance_done: label
advance_done:

// 8. hide_cards():
lv_obj_add_flag(lm2_card, LV_OBJ_FLAG_HIDDEN);
lv_obj_add_flag(lm3_card, LV_OBJ_FLAG_HIDDEN);

// 9. NAV_PAGE tap handler:
if (strcmp(st.stats.p[st.nav_provider].id, "lmstudio") == 0) {
    st.nav_card = (card_kind_t)(((int)st.nav_card + 1) % 4);
    st.dirty = true;
} else {
    // existing 2-card toggle
}

// 10. Summary row tap:
if (strcmp(tp->id, "lmstudio") == 0) {
    st.nav_card = CARD_COST;
} else {
    st.nav_card = tp->has_cost ? CARD_COST : CARD_LIMITS;
}

// 11. Weekly bar condition:
if (((strcmp(p->id, "claude") == 0 || strcmp(p->id, "codex") == 0)
     && p->has_s)
    || strcmp(p->id, "lmstudio") == 0) {
```

### Success Criteria:

#### Automated Verification:
- [x] Type checking passes: `cd firmware && idf.py build`
- [x] `card_kind_t` enum has 4 entries: `CARD_COST`, `CARD_LIMITS`, `CARD_LM_STATS_2`, `CARD_LM_STATS_3`
- [x] `is_lmstudio` flag correctly set in `render_card()` based on `p->id`
- [x] Summary row shows `lmstudio` with dual bars (token % in row_bar, request % in row_bar_w)
- [x] Weekly bar (`row_bar_w`) shows for `lmstudio` provider
- [x] `is_hidden_provider()` does NOT include `lmstudio`
- [x] Tap on lmstudio summary row opens TODAY page (`CARD_COST`)
- [x] Screensaver cycles through LM Studio card types (4-card cycle in saver_advance_locked)
- [x] Provider activity tracking includes LM Studio (provider_metric_sig hashes lm_* fields)
- [x] provider_card_available() returns true for LM card types

#### Manual Verification:
- [ ] Summary list scroll works with all providers including LM Studio
- [ ] Purple accent color (`#7C3AED`) appears on LM Studio bars and header
- [ ] "off" text displays when LM Studio has no data (`ok:false`)
- [ ] Screensaver enters and cycles through LM Studio cards correctly
- [ ] Tap on LM Studio row opens TODAY page first

---

## Phase 5: Card Pages

### Overview
Add 4 LM Studio card pages to the firmware UI: TODAY (hero tokens + requests + 30-day chart), STATS 1 (tokens % + requests %), MODELS table (Rank/Model/Req), and WEEK table (Date/Req/Toks/Cp%/Hit%). Add new widget variables for `lm2_card` and `lm3_card` with LVGL table widgets. Implement `render_card()` branches for `CARD_LM_STATS_2` and `CARD_LM_STATS_3`.

This phase depends on Phase 4 (needs `card_kind_t` +2 enums, nav context, `hide_cards()`).

### Changes Required:

#### 1. Firmware UI — Widget variables
**File**: `firmware/main/ui.c` — Add after lim_ser declaration:
```c
// LM Studio card 2: top-10 models table
static lv_obj_t *lm2_card, *lm2_hdr, *lm2_logo, *lm2_table;
// LM Studio card 3: 7-day daily usage table
static lv_obj_t *lm3_card, *lm3_hdr, *lm3_logo, *lm3_table;
```

#### 2. Firmware UI — build_widgets() additions (end of function, after lim_x_bar)
```c
    // ---- LM Studio card 2: top-10 models table ----
    lm2_card = lv_obj_create(scr);
    lv_obj_set_size(lm2_card, W, H);
    lv_obj_set_pos(lm2_card, 0, 0);
    lv_obj_set_style_bg_color(lm2_card, lv_color_hex(0x0b0b0b), 0);
    lv_obj_set_style_bg_opa(lm2_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(lm2_card, 0, 0);
    lv_obj_set_style_radius(lm2_card, 0, 0);
    lv_obj_set_style_pad_all(lm2_card, 0, 0);
    lv_obj_clear_flag(lm2_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(lm2_card, LV_OBJ_FLAG_HIDDEN);
    create_card_hdr(lm2_card, &lm2_hdr, &lm2_logo);
    lm2_table = lv_table_create(lm2_card);
    lv_obj_set_pos(lm2_table, 12, 34);
    lv_obj_set_size(lm2_table, W - 24, H - 44);
    lv_table_set_col_cnt(lm2_table, 3);
    lv_table_set_cell_value(lm2_table, 0, 0, "#");
    lv_table_set_cell_value(lm2_table, 0, 1, "Model");
    lv_table_set_cell_value(lm2_table, 0, 2, "Req");
    lv_obj_set_style_text_color(lm2_table, lv_color_hex(0xe8eaed), 0);
    lv_obj_set_style_border_width(lm2_table, 0, 0);
    lv_obj_set_style_radius(lm2_table, 0, 0);
    lv_obj_set_style_pad_all(lm2_table, 0, 0);

    // ---- LM Studio card 3: 7-day daily usage table ----
    lm3_card = lv_obj_create(scr);
    lv_obj_set_size(lm3_card, W, H);
    lv_obj_set_pos(lm3_card, 0, 0);
    lv_obj_set_style_bg_color(lm3_card, lv_color_hex(0x0b0b0b), 0);
    lv_obj_set_style_bg_opa(lm3_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(lm3_card, 0, 0);
    lv_obj_set_style_radius(lm3_card, 0, 0);
    lv_obj_set_style_pad_all(lm3_card, 0, 0);
    lv_obj_clear_flag(lm3_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(lm3_card, LV_OBJ_FLAG_HIDDEN);
    create_card_hdr(lm3_card, &lm3_hdr, &lm3_logo);
    lm3_table = lv_table_create(lm3_card);
    lv_obj_set_pos(lm3_table, 12, 34);
    lv_obj_set_size(lm3_table, W - 24, H - 44);
    lv_table_set_col_cnt(lm3_table, 5);
    lv_table_set_cell_value(lm3_table, 0, 0, "Date");
    lv_table_set_cell_value(lm3_table, 0, 1, "Req");
    lv_table_set_cell_value(lm3_table, 0, 2, "Toks");
    lv_table_set_cell_value(lm3_table, 0, 3, "Cp%");
    lv_table_set_cell_value(lm3_table, 0, 4, "Hit%");
    lv_obj_set_style_text_color(lm3_table, lv_color_hex(0xe8eaed), 0);
    lv_obj_set_style_border_width(lm3_table, 0, 0);
    lv_obj_set_style_radius(lm3_table, 0, 0);
    lv_obj_set_style_pad_all(lm3_table, 0, 0);
```

#### 3. Firmware UI — CARD_COST TODAY branch (inside cost card block, after `lv_obj_add_flag(cost_na, LV_OBJ_FLAG_HIDDEN)`)
```c
        if (is_lmstudio) {
            lv_obj_add_flag(cost_na, LV_OBJ_FLAG_HIDDEN);
            lv_obj_t *hide[] = { cost_or_lbl, cost_or_row1, cost_or_row2,
                                 cost_bar, cost_bar_lbl, cost_cap };
            for (unsigned i = 0; i < sizeof hide / sizeof *hide; i++)
                lv_obj_add_flag(hide[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_t *show[] = { cost_big, cost_tok, cost_tok_unit, cost_30, cost_chart };
            for (unsigned i = 0; i < sizeof show / sizeof *show; i++)
                lv_obj_clear_flag(show[i], LV_OBJ_FLAG_HIDDEN);
            const int scr_w = lv_display_get_horizontal_resolution(lv_display_get_default());
            const int scr_h = lv_display_get_vertical_resolution(lv_display_get_default());
            lv_obj_set_pos(cost_30, 12, scr_h - 22);
            lv_obj_set_size(cost_chart, scr_w - 24, scr_h - 150);
            char tk[16], rq[16], tk30[16], rq30[16];
            fmt_tokens(tk, sizeof tk, p->lm_tok_today);
            snprintf(rq, sizeof rq, "%d", (int)p->lm_req_today);
            lv_label_set_text(cost_big, tk);
            lv_label_set_text(cost_tok, rq);
            lv_label_set_text(cost_tok_unit, "requests");
            lv_obj_align_to(cost_tok_unit, cost_tok, LV_ALIGN_OUT_RIGHT_BOTTOM, 5, 0);
            fmt_tokens(tk30, sizeof tk30, p->lm_tok_month_max);
            snprintf(rq30, sizeof rq30, "%d", (int)p->lm_req_month_max);
            lv_label_set_text_fmt(cost_30, "30 DAY MAX: %s Toks " LV_SYMBOL_BULLET "  %s Reqs", tk30, rq30);
            int n = p->lm_ht_n;
            if (n > NAV_HIST_PTS) n = NAV_HIST_PTS;
            lv_color_t cc;
            int32_t ht32[STATS_HIST_MAX];
            for (int hi = 0; hi < n && hi < STATS_HIST_MAX; hi++)
                ht32[hi] = (int32_t)(p->lm_ht[hi] > INT32_MAX ? INT32_MAX : p->lm_ht[hi]);
            int32_t mx = render_cost_bar_chart(cost_chart, cost_ser, ht32, n,
                prov_accent(p->id, &cc) ? cc : lv_color_hex(0x7C3AED));
            if (card_entered) anim_chart_fadein(cost_chart);
            return;
        }
```

#### 4. Firmware UI — CARD_COST guard (bypass cost_na for LM Studio)
Replace `if (!p->has_cost)` with:
```c
        if (!p->has_cost && !p->has_lm) {
```

#### 5. Firmware UI — STATS 1 (inside CARD_LIMITS, before existing limits code)
```c
    char pb[12];
    if (is_lmstudio) {
        // LM Studio STATS 1: tokens % + requests %
        render_card_hdr(lim_hdr, lim_logo, p->id, "STATS 1");
        lv_obj_add_flag(lim_a_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lim_a_big, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lim_a_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lim_a_rst, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lim_chart, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lim_x_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lim_x_val, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lim_x_bar, LV_OBJ_FLAG_HIDDEN);
        fmt_pct(pb, sizeof pb, p->has_lm, p->p);
        lv_label_set_text(lim_s_lbl, "TOKENS");
        if (card_entered && p->has_lm) {
            anim_count_up(lim_s_big, (int32_t)(p->p * 10.0f + 0.5f), count_pct_cb);
        } else {
            lv_label_set_text(lim_s_big, pb);
        }
        set_bar(lim_s_bar, p->has_lm, p->p, p);
        lv_obj_add_flag(lim_s_rst, LV_OBJ_FLAG_HIDDEN);
        // Weekly = requests %
        fmt_pct(pb, sizeof pb, p->has_lm, p->s);
        if (p->has_lm) {
            lv_obj_clear_flag(lim_w_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lim_w_big, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lim_w_bar, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(lim_w_lbl, "REQUESTS");
            lv_label_set_text(lim_w_big, pb);
            set_bar(lim_w_bar, true, p->s, p);
        } else {
            lv_obj_add_flag(lim_w_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lim_w_big, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lim_w_bar, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_add_flag(lim_w_rst, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    render_card_hdr(lim_hdr, lim_logo, p->id, "LIMITS");
    // ... existing limits card code ...
```

#### 6. Firmware UI — CARD_LM_STATS_2 (models table, between CARD_COST return and CARD_LIMITS)
```c
    if (st.nav_card == CARD_LM_STATS_2 && is_lmstudio) {
        lv_obj_add_flag(cost_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lim_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lm2_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lm3_card, LV_OBJ_FLAG_HIDDEN);
        render_card_hdr(lm2_hdr, lm2_logo, p->id, "MODELS");
        const int scr_w = lv_display_get_horizontal_resolution(lv_display_get_default());
        int n = p->lm_models_n;
        lv_table_set_row_cnt(lm2_table, (uint32_t)(n > 0 ? n + 1 : 1));
        for (int i = 1; i <= n && i < LM_MODELS_MAX; i++) {
            char rank[4], rq[12];
            snprintf(rank, sizeof rank, "%d", i + 1);
            snprintf(rq, sizeof rq, "%d", (int)p->lm_models_req[i]);
            lv_table_set_cell_value(lm2_table, i, 0, rank);
            lv_table_set_cell_value(lm2_table, i, 1, p->lm_models_id[i]);
            lv_table_set_cell_value(lm2_table, i, 2, rq);
        }
        if (n == 0) {
            lv_table_set_cell_value(lm2_table, 1, 0, "--");
            lv_table_set_cell_value(lm2_table, 1, 1, "no models");
            lv_table_set_cell_value(lm2_table, 1, 2, "--");
        }
        lv_table_set_col_width(lm2_table, 0, 36);
        lv_table_set_col_width(lm2_table, 1, scr_w - 110);
        lv_table_set_col_width(lm2_table, 2, 50);
        return;
    }

    // LM Studio card 3: 7-day daily usage table
    if (st.nav_card == CARD_LM_STATS_3 && is_lmstudio) {
        lv_obj_add_flag(cost_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lim_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lm2_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lm3_card, LV_OBJ_FLAG_HIDDEN);
        render_card_hdr(lm3_hdr, lm3_logo, p->id, "WEEK");
        const int scr_w = lv_display_get_horizontal_resolution(lv_display_get_default());
        int n = p->lm_week_n;
        lv_table_set_row_cnt(lm3_table, (uint32_t)(n > 0 ? n + 1 : 1));
        for (int i = 1; i <= n && i < LM_WEEK_MAX; i++) {
            lv_table_set_cell_value(lm3_table, i, 0, p->lm_week_d[i]);
            char rq_str[12], tk_str[12], cp_str[8], ch_str[8];
            snprintf(rq_str, sizeof rq_str, "%d", (int)p->lm_week_rq[i]);
            lv_table_set_cell_value(lm3_table, i, 1, rq_str);
            fmt_tokens(tk_str, sizeof tk_str, p->lm_week_tk[i]);
            lv_table_set_cell_value(lm3_table, i, 2, tk_str);
            if (p->lm_week_cp[i] > 0)
                snprintf(cp_str, sizeof cp_str, "%.0f%%", (double)p->lm_week_cp[i]);
            else
                snprintf(cp_str, sizeof cp_str, "N/A");
            lv_table_set_cell_value(lm3_table, i, 3, cp_str);
            if (p->lm_week_ch[i] > 0)
                snprintf(ch_str, sizeof ch_str, "%.0f%%", (double)p->lm_week_ch[i]);
            else
                snprintf(ch_str, sizeof ch_str, "N/A");
            lv_table_set_cell_value(lm3_table, i, 4, ch_str);
        }
        if (n == 0) {
            for (int col = 0; col < 5; col++)
                lv_table_set_cell_value(lm3_table, 1, col, "--");
        }
        lv_table_set_col_width(lm3_table, 0, 48);
        lv_table_set_col_width(lm3_table, 1, 48);
        lv_table_set_col_width(lm3_table, 2, 54);
        lv_table_set_col_width(lm3_table, 3, 48);
        lv_table_set_col_width(lm3_table, 4, 48);
        return;
    }
```

### Success Criteria:

#### Automated Verification:
- [x] Type checking passes: `cd firmware && idf.py build`
- [x] All new card widgets (`lm2_card`, `lm3_card`, `lm2_hdr`, `lm2_logo`, `lm2_table`, `lm3_hdr`, `lm3_logo`, `lm3_table`) are created in `build_widgets()`
- [x] `render_card()` handles `CARD_LM_STATS_2` (models table) and `CARD_LM_STATS_3` (week table) cases
- [x] `hide_cards()` hides `lm2_card` and `lm3_card`
- [x] `provider_card_available()` returns `true` for LM card types when `has_lm`
- [x] `saver_candidate_at()` selects initial card `CARD_COST` for LM Studio
- [x] Tap cycles through 4 cards: TODAY → STATS 1 → MODELS → WEEK → TODAY
- [x] LM Studio TODAY bypasses `!p->has_cost` guard via `!p->has_cost && !p->has_lm`
- [x] `int64_t lm_ht[]` properly converted to int32_t for chart renderer

#### Manual Verification:
- [ ] TODAY page shows: hero token count (lemonmilk_48), smaller hero request count, 30-day token bar chart, "30 DAY MAX: X Toks • Y Reqs" at bottom
- [ ] STATS 1 page shows: TOKENS hero % (session style), REQUESTS sub-hero % (weekly style)
- [ ] MODELS page shows: top-10 models table (Rank, Model name, Request count) using LVGL table widget
- [ ] WEEK page shows: 7-day daily table (Date, Req, Toks, Cp%, Hit%) using LVGL table widget
- [ ] Cache % and Cache Hit % show 'N/A' when data unavailable
- [ ] Card transitions are smooth (no missing frames)
- [ ] Cards render correctly in both landscape (W=320) and portrait (W=240) orientations
- [ ] Table column widths fit within screen width
- [ ] LEMONMILK font renders U+002D hyphen, %, digits 0-9, and date separators on device

---

## Testing Strategy

### Automated:
- From Phase 1: `cd firmware && idf.py build`, `python3 -m json.tool docs/generated/codexbar-payload.schema.json`
- From Phase 2: `./scripts/lmstudio-stats.sh | python3 -m json.tool`
- From Phase 3: `bash -n scripts/codexbar-publish.sh`
- From Phases 4-5: `cd firmware && idf.py build`

### Manual Testing Steps:
1. Run `./scripts/lmstudio-stats.sh` against a real LM Studio server-logs directory — verify JSON output
2. Run `codexbar-publish.sh --once` with LM Studio running — verify LM provider in payload
3. Flash firmware and observe LM Studio row on summary + verify 4 card pages via tap cycling
4. Verify screensaver cycles LM Studio cards after period of inactivity
5. Verify N/A display when cache data absent

## Performance Considerations
- Host script must complete within 10s (parsing up to ~50 log files per day, each up to ~10MB)
- Rolling history file is a single JSON file, read/updated every 5 min publish cycle
- Firmware: 4 new card pages must render within existing 5ms tick cycle
- New `stats_provider_t` grows by ~420 bytes, total ~2.3 KB (still fits in 4KB fetch buffer)
- LM Studio card widgets are created once in `build_widgets()` and shown/hidden — no dynamic allocation

## Migration Notes
Not applicable — no existing data schema to migrate. LM Studio logs are ephemeral (rotated by LM Studio); the rolling history file is created fresh on first run.

## Developer Context

## Plan Review (Step 4)

_Independent post-finalization review by artifact-code-reviewer and artifact-coverage-reviewer subagents. Findings triaged at Step 5._

| source   | plan-loc          | codebase-loc                | severity   | dimension             | finding   | recommendation   | resolution         |
| -------- | ----------------- | --------------------------- | ---------- | --------------------- | --------- | ---------------- | ------------------ |
| code     | Phase 5 §2        | ui.c:1329-1336              | blocker    | code-quality          | Data loop overwrites header row (lm2_table) | lv_table_set_row_cnt(n+1), start at row 1 | `applied: row count n+1, loop starts at 1` |
| code     | Phase 5 §2        | ui.c:1358-1376              | blocker    | code-quality          | Same header-overwrite bug on lm3_table     | lv_table_set_row_cnt(n+1), start at row 1 | `applied: row count n+1, loop starts at 1` |
| code     | Phase 3 §1        | schema.json:44             | concern    | codebase-fit          | Schema requires hr/ht but JXA conditionally drops | Remove hr/ht from required array | `applied: removed from required` |
| code     | Phase 5 §2        | ui.c:1329-1343              | concern    | code-quality          | Empty-data fallback overwrites header      | Write N/A at row 1, not row 0          | `applied: row 1 for N/A fallback` |
| coverage | ## Verification Notes §3 | <n/a> | concern | verification-coverage | No Success Criteria for font glyph coverage | Add Phase 5 Manual bullet for LEMONMILK glyphs | `applied: bullet added` |
| code     | Phase 4 §1        | ui.c:354-368                | suggestion | code-quality          | saver_advance_locked uses activity[] scan   | Use find_provider_id() instead         | `applied: updated to find_provider_id()` |
| code     | Phase 4 §5        | ui.c:175-187                | suggestion | code-quality          | Sig missing lm_models_n/lm_week_n hashing  | Add hash_mix_u32 for both fields        | `applied: added model_n and week_n hashing` |(To be filled during Step 5 if needed)

## References
- Design: `.rpiv/artifacts/designs/2026-05-23_12-04-10_lm-studio-stats-on-device.md`
- Research: `.rpiv/artifacts/research/2026-05-23_11-34-39_lm-studio-stats-on-device.md`
- Discover: `.rpiv/artifacts/discover/2026-05-23_10-41-20_lm-studio-stats.md`