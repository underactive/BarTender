#!/bin/zsh
# ollama-stats.sh — reduce local Ollama inference logs into the compact
# provider object merged by codexbar-publish.sh.
#
#   ollama-stats.sh             # emits one JSON provider object for id="ollama"
#   ollama-stats.sh --help
#
# Reads local Ollama server logs only:
#   ~/Library/Application Support/Ollama/logs/server.log         Ollama server log
#   ~/.config/codexbar-toy/ollama-history.json                    rolling 30-day history
#
# Output is privacy-reduced and contains no raw prompts, conversation context,
# response IDs, or API keys. Only daily-aggregated counts and percentages:
#   {"id":"ollama","ok":true,"p":45.3,"s":60.0,
#    "ol":{"rq":42,"tk":123456,"mxr":100,"mxt":500000,
#          "hr":[...],"ht":[...],"week":[...]}}
#
# Field units:
#   p         tokens % vs 30-day max daily tokens
#   s         requests % vs 30-day max daily requests
#   ol.rq     requests today
#   ol.tk     tokens today
#   ol.mxr    30-day max daily requests
#   ol.mxt    30-day max daily tokens
#   ol.hr[]   daily requests history, oldest -> newest, up to 31 points
#   ol.ht[]   daily tokens history, oldest -> newest, up to 31 points
#   ol.week[] 7-day daily table [{d,rq,tk}]
#
# Env overrides (testability hooks; default to real user paths):
#   OLLAMA_LOG_DIR      default: ~/Library/Application Support/Ollama/logs
#   OLLAMA_HISTORY      default: ~/.config/codexbar-toy/ollama-history.json
#   PYTHON3             Python interpreter (default: command -v python3)
#
# Fail-soft contract: exits non-zero when no usable local Ollama usage
# exists; the publisher logs the skip and continues with the payload unchanged.
set -u

help() { awk 'NR>=2 && /^#/{sub(/^# ?/,"");print;next} NR>=2{exit}' "$0"; }
case "${1:-}" in
  "") ;;
  -h|--help) help; exit 0 ;;
  *) print -r -- "unknown argument: $1 (try --help)" >&2; exit 2 ;;
esac

PY="${PYTHON3:-$(command -v python3 2>/dev/null || true)}"
[[ -n "$PY" && -x "$PY" ]] || { print -r -- "ollama-stats: python3 not found" >&2; exit 2; }

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
import _stats_history as _hist  # shared rolling-history helpers


def eprint(msg: str) -> None:
    print(f"ollama-stats: {msg}", file=sys.stderr)


# Environment overrides
home = Path.home()
log_dir = Path(os.environ.get(
    "OLLAMA_LOG_DIR",
    home / "Library" / "Application Support" / "Ollama" / "logs"
)).expanduser()
hist_file = Path(os.environ.get(
    "OLLAMA_HISTORY",
    home / ".config" / "codexbar-toy" / "ollama-history.json"
)).expanduser()

# Regex patterns for Ollama server log
# Ollama logs look like: time=YYYY-MM-DDTHH:MM:SS...
# Chat completion requests via /api/chat
RE_REQUEST = re.compile(
    r'time=(\d{4}-\d{2}-\d{2})T\d{2}:\d{2}:\d{2}'
)
RE_CHAT = re.compile(r'/api/chat')
# Token counts from "eval count" or "prompt eval count" in Ollama logs
RE_EVAL_COUNT = re.compile(r'eval count=(\d+)')
RE_PROMPT_EVAL_COUNT = re.compile(r'prompt eval count=(\d+)')


def parse_log(path: str) -> dict:
    """Parse an Ollama server log file and return daily metrics."""
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        content = f.read()

    today = dt.date.today()
    today_str = today.isoformat()

    requests = 0
    total_tokens = 0

    for line in content.split('\n'):
        if not line.strip():
            continue
        m = RE_REQUEST.search(line)
        if not m:
            continue
        date_str = m.group(1)
        if date_str != today_str:
            continue
        if RE_CHAT.search(line):
            requests += 1
        em = RE_EVAL_COUNT.search(line)
        if em:
            total_tokens += int(em.group(1))
        pem = RE_PROMPT_EVAL_COUNT.search(line)
        if pem:
            total_tokens += int(pem.group(1))

    return {
        "date": today_str,
        "requests": requests,
        "tokens": total_tokens,
    }


def parse_all_logs(log_dir: Path) -> dict:
    """Parse all log files in the log directory, building daily aggregates."""
    results = {}
    today = dt.date.today()

    if not log_dir.is_dir():
        eprint(f"log dir absent: {log_dir}")
        return results

    for log_file in sorted(log_dir.iterdir()):
        if not log_file.is_file():
            continue
        # Ollama names logs as server.log or server-YYYY-MM-DD.log
        if not (log_file.name == "server.log" or log_file.name.startswith("server-")):
            continue

        try:
            stats = parse_log(str(log_file))
            date_key = stats["date"]
            if date_key in results:
                results[date_key]["requests"] += stats["requests"]
                results[date_key]["tokens"] += stats["tokens"]
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
parsed = parse_all_logs(log_dir)
today_str = today.isoformat()

today_data = parsed.get(today_str, {
    "date": today_str, "requests": 0, "tokens": 0
})

# ---------------------------------------------------------------------------
# 2. Read/update rolling history file
# ---------------------------------------------------------------------------
history = _hist.load_history(hist_file)

# Merge today's data into history
if today_str not in history:
    history[today_str] = {
        "rq": today_data["requests"],
        "tk": today_data["tokens"],
    }
else:
    existing = history[today_str]
    # Only overwrite if today's data changed
    if existing.get("rq", 0) != today_data["requests"] or \
       existing.get("tk", 0) != today_data["tokens"]:
        history[today_str] = {
            "rq": today_data["requests"],
            "tk": today_data["tokens"],
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
# 6. Determine ok flag
# ---------------------------------------------------------------------------
any_data = today_data["requests"] > 0 or today_data["tokens"] > 0
has_history = any(v.get("rq", 0) > 0 or v.get("tk", 0) > 0 for v in history.values())
ok = any_data or has_history
if not ok:
    eprint("no usable Ollama data")
    provider = {"id": "ollama", "ok": False}
    print(json.dumps(provider, separators=(",", ":")))
    sys.exit(3)

# ---------------------------------------------------------------------------
# 7. Build and emit provider JSON
# ---------------------------------------------------------------------------
provider = {
    "id": "ollama",
    "ok": True,
    "p": tok_pct,
    "s": req_pct,
    "ol": {
        "rq": today_data["requests"],
        "tk": today_data["tokens"],
        "mxr": max_req,
        "mxt": max_tok,
        "hr": req_hist[-31:],
        "ht": tok_hist[-31:],
        "week": week_entries[-7:],
    },
}

print(json.dumps(provider, separators=(",", ":")))
eprint(
    f"today: {today_data['requests']}rq/{today_data['tokens']}tok "
    f"max: {max_req}rq/{max_tok}tok "
    f"hist: {len(req_hist)}d "
    f"week: {len(week_entries)}d"
)
PY