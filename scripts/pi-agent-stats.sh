#!/bin/zsh
# pi-agent-stats.sh — reduce local Pi Agent session state into the compact
# provider object merged by codexbar-publish.sh.
#
#   pi-agent-stats.sh          # emits one JSON provider object for id="pi"
#   pi-agent-stats.sh --help
#
# Reads local Pi Agent state only:
#   ~/.pi/agent/sessions/**/*.jsonl   session event logs with assistant usage
#   ~/.pi/agent/models.json           local/custom provider config (read-only)
#
# Output is privacy-reduced and contains no prompts, cwd/project paths, model
# names, provider credentials, response IDs, or raw session rows:
#   {"id":"pi","ok":true,"p":41.2,"pi":{"ps":1245,"pt":893421,"h":[...]}}
#
# Field units:
#   p      latest-day activity as % of 30-day max spend (or max tokens if spend=0)
#   pi.ps  max daily spend over the last 30 calendar days, integer cents
#   pi.pt  max daily tokens over the last 30 calendar days
#   pi.h   daily spend history, integer cents, oldest -> newest, 30 points
#
# Env overrides (testability hooks; default to real Pi Agent paths):
#   PI_AGENT_HOME          default: ~/.pi/agent
#   PI_AGENT_SESSIONS_DIR  default: $PI_AGENT_HOME/sessions
#   PI_AGENT_MODELS_FILE   default: $PI_AGENT_HOME/models.json
#   PYTHON3                Python interpreter (default: command -v python3)
#
# Fail-soft contract: exits non-zero when no usable local Pi usage exists; the
# publisher logs the skip and continues with the CodexBar payload unchanged.
set -u

help() { awk 'NR>=2 && /^#/{sub(/^# ?/,"");print;next} NR>=2{exit}' "$0"; }
case "${1:-}" in
  "") ;;
  -h|--help) help; exit 0 ;;
  *) print -r -- "unknown argument: $1 (try --help)" >&2; exit 2 ;;
esac

PY="${PYTHON3:-$(command -v python3 2>/dev/null || true)}"
[[ -n "$PY" && -x "$PY" ]] || { print -r -- "pi-agent-stats: python3 not found" >&2; exit 2; }

"$PY" <<'PY'
import datetime as dt
import json
import os
import sys
from pathlib import Path


def eprint(msg: str) -> None:
    print(f"pi-agent-stats: {msg}", file=sys.stderr)


def number(value):
    if value is None:
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def usage_cost_dollars(usage: dict) -> float:
    cost = usage.get("cost")
    n = number(cost)
    if n is not None:
        return n
    if not isinstance(cost, dict):
        return 0.0
    total = number(cost.get("total"))
    if total is not None:
        return total
    parts = ["input", "output", "cacheRead", "cacheWrite", "cacheCreate", "cached"]
    vals = [number(cost.get(k)) for k in parts]
    return sum(v for v in vals if v is not None)


def usage_tokens(usage: dict) -> int:
    total = number(usage.get("totalTokens"))
    if total is not None:
        return int(round(total))
    parts = ["input", "output", "cacheRead", "cacheWrite", "cacheCreate", "cached"]
    vals = [number(usage.get(k)) for k in parts]
    return int(round(sum(v for v in vals if v is not None)))


def usage_objects(row: dict):
    # Current Pi Agent JSONL rows put usage at message.usage. Keep a few shallow
    # wrapper fallbacks for format churn, but do not recursively traverse prompt
    # content arrays/strings or publish any raw fields.
    candidates = []
    msg = row.get("message")
    if isinstance(msg, dict) and isinstance(msg.get("usage"), dict):
        candidates.append(msg["usage"])
    if isinstance(row.get("usage"), dict):
        candidates.append(row["usage"])
    data = row.get("data")
    if isinstance(data, dict):
        dmsg = data.get("message")
        if isinstance(dmsg, dict) and isinstance(dmsg.get("usage"), dict):
            candidates.append(dmsg["usage"])
    event = row.get("event")
    if isinstance(event, dict):
        emsg = event.get("message")
        if isinstance(emsg, dict) and isinstance(emsg.get("usage"), dict):
            candidates.append(emsg["usage"])
    return candidates


def timestamp_day(row: dict):
    msg = row.get("message") if isinstance(row.get("message"), dict) else {}
    raw = row.get("timestamp") or msg.get("timestamp")
    if not raw:
        return None
    s = str(raw)
    try:
        parsed = dt.datetime.fromisoformat(s.replace("Z", "+00:00"))
    except ValueError:
        return None
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=dt.timezone.utc)
    return parsed.astimezone(dt.timezone.utc).date()


home = Path.home()
agent_home = Path(os.environ.get("PI_AGENT_HOME", home / ".pi" / "agent")).expanduser()
sessions_dir = Path(os.environ.get("PI_AGENT_SESSIONS_DIR", agent_home / "sessions")).expanduser()
models_file = Path(os.environ.get("PI_AGENT_MODELS_FILE", agent_home / "models.json")).expanduser()

# Read models.json to keep the helper tied to the same local Pi Agent config
# surface, but never publish model/provider names or credentials. Session usage
# rows already carry reduced cost/tokens, so models are not needed for pricing.
try:
    if models_file.is_file():
        json.loads(models_file.read_text(encoding="utf-8"))
except Exception:
    eprint("models.json parse skipped")

if not sessions_dir.is_dir():
    eprint(f"sessions dir absent: {sessions_dir}")
    sys.exit(3)

today = dt.datetime.now(dt.timezone.utc).date()
dates = [today - dt.timedelta(days=i) for i in range(29, -1, -1)]
window = {d: {"dollars": 0.0, "tokens": 0} for d in dates}
start, end = dates[0], dates[-1]

files = rows = used_rows = 0
for path in sessions_dir.rglob("*.jsonl"):
    # Fast filename-date prune for the common Pi Agent naming convention:
    # 2026-05-20T22-42-37-535Z_<id>.jsonl. If no date prefix is present,
    # still scan the file because the row timestamp is authoritative.
    try:
        prefix = path.name[:10]
        if len(prefix) == 10 and prefix[4] == "-" and prefix[7] == "-":
            fday = dt.date.fromisoformat(prefix)
            if fday < start or fday > end:
                continue
    except ValueError:
        pass
    files += 1
    try:
        with path.open("r", encoding="utf-8") as fh:
            for line in fh:
                if '"usage"' not in line:
                    continue
                rows += 1
                try:
                    row = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if not isinstance(row, dict):
                    continue
                day = timestamp_day(row)
                if day not in window:
                    continue
                for usage in usage_objects(row):
                    dollars = usage_cost_dollars(usage)
                    tokens = usage_tokens(usage)
                    if dollars <= 0 and tokens <= 0:
                        continue
                    window[day]["dollars"] += dollars
                    window[day]["tokens"] += tokens
                    used_rows += 1
    except OSError:
        continue

if files == 0:
    eprint(f"no session jsonl files in {sessions_dir}")
    sys.exit(3)

hist = []
max_spend = 0
max_tokens = 0
latest_spend = 0
latest_tokens = 0
any_usage = False
for day in dates:
    vals = window[day]
    cents = int(round(vals["dollars"] * 100))
    tokens = int(round(vals["tokens"]))
    hist.append(cents)
    max_spend = max(max_spend, cents)
    max_tokens = max(max_tokens, tokens)
    any_usage = any_usage or cents > 0 or tokens > 0
    latest_spend, latest_tokens = cents, tokens

if not any_usage:
    eprint(f"no usable Pi usage in last 30 days (files={files}, rows={rows})")
    sys.exit(3)

if max_spend > 0:
    pct = round((latest_spend * 100.0) / max_spend, 1)
elif max_tokens > 0:
    pct = round((latest_tokens * 100.0) / max_tokens, 1)
else:
    pct = 0.0
pct = max(0.0, min(100.0, pct))

provider = {"id": "pi", "ok": True, "p": pct, "pi": {"ps": max_spend, "pt": max_tokens, "h": hist}}
print(json.dumps(provider, separators=(",", ":")))
eprint(f"reduced {used_rows} usage rows from {files} files: max={max_spend}c/{max_tokens}tok hist={len(hist)}d")
PY
