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

# Heredoc-piped Python has no __file__, so hand it this script's dir to import
# scripts/lib/_stats_history.py (${0:A:h} = bin dir in zsh).
export CBTOY_SCRIPT_DIR="${0:A:h}/lib"
"$PY" <<'PY'
import datetime as dt
import json
import os
import re
import sys
from pathlib import Path

sys.path.insert(0, os.environ.get("CBTOY_SCRIPT_DIR", "."))
import _stats_history as _hist  # shared rolling-history helpers (Fowler audit #8)


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
        # Fallback: slot-selection-based estimate
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

    # Walk year-month subdirectories
    if not log_dir.is_dir():
        eprint(f"log dir absent: {log_dir}")
        return results

    for year_dir in sorted(log_dir.iterdir()):
        if not year_dir.is_dir():
            continue
        for log_file in sorted(year_dir.iterdir()):
            if not log_file.name.endswith('.log'):
                continue
            # Extract date from filename: YYYY-MM-DD.N.log
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
                # Merge: accumulate requests and tokens across rotated logs for same day
                if date_key in results:
                    results[date_key]["requests"] += stats["requests"]
                    results[date_key]["tokens"] += stats["tokens"]
                    # Use latest cache pct/hit from the last log of the day
                    if stats["cache_pct"] > 0:
                        results[date_key]["cache_pct"] = stats["cache_pct"]
                    if stats["cache_hit_pct"] > 0:
                        results[date_key]["cache_hit_pct"] = stats["cache_hit_pct"]
                    # Merge model counts
                    for m, c in stats["model_counts"].items():
                        results[date_key]["model_counts"][m] = \
                            results[date_key]["model_counts"].get(m, 0) + c
                else:
                    results[date_key] = stats
            except Exception as e:
                eprint(f"error parsing {log_file}: {e}")
                continue

    return results


# ---------------------------------------------------------------------------
# 1. Parse today's log files
# ---------------------------------------------------------------------------
today = dt.date.today()
parsed = parse_recent_logs(log_dir, days=31)
today_str = today.isoformat()

today_data = parsed.get(today_str)
if today_data is None or (today_data["requests"] == 0 and today_data["tokens"] == 0):
    eprint(f"no usable data for {today_str}")
    # Still try to serve history if available
    today_data = {"date": today_str, "requests": 0, "tokens": 0,
                  "cache_pct": 0.0, "cache_hit_pct": 0.0, "model_counts": {}}

# ---------------------------------------------------------------------------
# 2. Read/update rolling history file
# ---------------------------------------------------------------------------
history = _hist.load_history(hist_file)

# Merge today's data into history
# On first run (empty history), seed from all parsed logs
if not history and parsed:
    for date_key, day_data in parsed.items():
        history[date_key] = {
            "rq": day_data["requests"],
            "tk": day_data["tokens"],
            "cp": day_data["cache_pct"],
            "ch": day_data["cache_hit_pct"],
        }
    eprint(f"seeded history from {len(parsed)} days of log data")

if today_str not in history:
    history[today_str] = {
        "rq": today_data["requests"],
        "tk": today_data["tokens"],
        "cp": today_data["cache_pct"],
        "ch": today_data["cache_hit_pct"],
    }
else:
    existing = history[today_str]
    # Only overwrite if today's data changed (handles zero-request days correctly)
    if existing.get("rq", 0) != today_data["requests"] or \
       existing.get("tk", 0) != today_data["tokens"]:
        history[today_str] = {
            "rq": today_data["requests"],
            "tk": today_data["tokens"],
            "cp": today_data["cache_pct"],
            "ch": today_data["cache_hit_pct"],
        }

# Prune to 31 days
history = _hist.prune_history(history, 31)

# ---------------------------------------------------------------------------
# 3. Write history back (atomic tmp+replace via shared helper)
# ---------------------------------------------------------------------------
_hist.save_history(hist_file, history)

# ---------------------------------------------------------------------------
# 4. Compute daily arrays and max values from history (30-day window)
# ---------------------------------------------------------------------------
# Sorted oldest -> newest
sorted_dates_asc = sorted(k for k in history.keys() if k <= today_str)

req_hist = []
tok_hist = []
max_req = 0
max_tok = 0

# Also build 7-day week table
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
            "d": d[-5:],  # "MM-DD"
            "rq": r,
            "tk": t,
            "cp": entry.get("cp", 0.0),
            "ch": entry.get("ch", 0.0),
        })

# ---------------------------------------------------------------------------
# 5. Compute percentages
# ---------------------------------------------------------------------------
tok_pct = 0.0
req_pct = 0.0
if max_tok > 0:
    tok_pct = round((today_data["tokens"] * 100.0) / max_tok, 1)
if max_req > 0:
    req_pct = round((today_data["requests"] * 100.0) / max_req, 1)
tok_pct = max(0.0, min(100.0, tok_pct))
req_pct = max(0.0, min(100.0, req_pct))

# ---------------------------------------------------------------------------
# 6. Build top-10 model list (today only)
# ---------------------------------------------------------------------------
top_models = sorted(today_data["model_counts"].items(), key=lambda x: -x[1])[:10]
model_list = [{"id": m, "rq": c} for m, c in top_models]

# ---------------------------------------------------------------------------
# 7. Determine ok flag
# ---------------------------------------------------------------------------
any_data = today_data["requests"] > 0 or today_data["tokens"] > 0
# Also consider history: if there's history but no new data today, still ok
has_history = any(v.get("rq", 0) > 0 or v.get("tk", 0) > 0 for v in history.values())
ok = any_data or has_history
if not ok:
    eprint("no usable LM Studio data")
    provider = {"id": "lmstudio", "ok": False}
    print(json.dumps(provider, separators=(",", ":")))
    sys.exit(3)

# ---------------------------------------------------------------------------
# 8. Build and emit provider JSON
# ---------------------------------------------------------------------------
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