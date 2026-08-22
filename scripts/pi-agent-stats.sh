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
#   {"id":"pi","ok":true,"p":41.2,"pi":{"ts":512,"tt":123456,"ps":1245,"pt":893421,"h":[...],"ht":[...]},
#    "derived":[{"id":"moonshot","tt":0,"ht":[...]},{"id":"qwencloud","tt":0,"tm":9217336}]}
#
# DERIVED PROVIDER SLICES (`derived`)
# Moonshot and Qwen Cloud expose no token-usage API at all — Moonshot's API is
# balance-only and Qwen Cloud bills in Credits, which are not tokens. Pi Agent
# session rows DO carry `message.provider` alongside per-turn token counts, so
# usage routed through Pi can be attributed back to those providers. This is
# the only token signal available for them.
#
# It is a KNOWN UNDERCOUNT. It sees only traffic that went through Pi Agent;
# usage from any other client is invisible. Measured against DeepSeek, the one
# provider where a real dashboard total exists to check against, Pi accounted
# for 41% of August tokens (72.3M of 178.2M) and showed zero on 12 of 21 days
# that had real activity. Treat these two numbers as a floor, never as the
# provider's true usage, and never as a basis for reconciling spend.
#
# Matching is on the Pi PROVIDER id, never the model name: kimi-* and qwen-*
# models also run through `opencode-go` and `ramp-router`, which are billed by
# those services and already tracked as their own providers. Attributing them
# to Moonshot/Qwen would double-count across providers.
#
# Field choice is driven by what each card actually renders:
#   moonshot   publishes cost.cr, so it draws the balance card — tt fills the
#              token row and ht[] the 30-day chart.
#   qwencloud  publishes no balance, so it draws the standard card, which
#              charts cost.h (spend) rather than ht and shows a 30-day token
#              total — hence tt + tm, and no ht that would render nothing.
# Spend is deliberately NOT derived for Moonshot: its card shows the real
# account balance one row below, and an undercounted SPEND beside an accurate
# balance would contradict itself on screen.
#
# Field units:
#   p      today's usage as % of the prior 29-day peak (or peak tokens if
#          spend=0); exceeds 100 when today beats that prior peak. Note: the
#          firmware summary bar ignores `p` for Pi and uses ht/tt instead
#          (today vs 30-day daily average, excluding 0-use days).
#   pi.ts  today's spend, integer cents
#   pi.tt  today's tokens
#   pi.ps  max daily spend over the last 30 calendar days, integer cents
#   pi.pt  max daily tokens over the last 30 calendar days
#   pi.h   daily spend history, integer cents, oldest -> newest, 30 points
#   pi.ht  daily token history, oldest -> newest, 30 points (sibling of h);
#          drives the summary bar's today-vs-average comparison
#
# All buckets key on the Mac's LOCAL calendar day, matching every other rollup
# in this pipeline. This helper previously bucketed in UTC, which rolled Pi's
# "today" over mid-afternoon in US timezones and disagreed with the Claude,
# Codex and OpenRouter tiles beside it.
#
# Note: 'p' is today vs the busiest prior day (excluding today), so a new
#       record can exceed 100%. ps/pt are the overall 30-day peaks.
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


# Pi provider-id prefix -> payload provider id. Prefix rather than exact match
# so a plan rename (qwen-token-plan-individual -> ...-team) does not silently
# drop the slice. Matching the provider, not the model, keeps kimi/qwen traffic
# billed by opencode-go or ramp-router out of these totals.
DERIVED_PREFIXES = (("moonshot", "moonshot"), ("qwen", "qwencloud"))


def derived_target(provider) -> str | None:
    if not isinstance(provider, str):
        return None
    p = provider.strip().lower()
    for prefix, target in DERIVED_PREFIXES:
        if p.startswith(prefix):
            return target
    return None


def row_provider(row: dict):
    msg = row.get("message")
    if isinstance(msg, dict) and msg.get("provider"):
        return msg.get("provider")
    return row.get("provider")


def timestamp_day(row: dict):
    """Bucket a session row on the LOCAL calendar day.

    Row timestamps are UTC-stamped, but every rollup in this pipeline keys on
    the Mac's local date. .astimezone() with no argument resolves the correct
    local offset per instant, so days spanning a DST change bucket correctly.
    """
    msg = row.get("message") if isinstance(row.get("message"), dict) else {}
    raw = row.get("timestamp") or msg.get("timestamp")
    if not raw:
        return None
    try:
        parsed = dt.datetime.fromisoformat(str(raw).replace("Z", "+00:00"))
    except ValueError:
        return None
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=dt.timezone.utc)
    return parsed.astimezone().date()


home = Path.home()
agent_home = Path(os.environ.get("PI_AGENT_HOME", home / ".pi" / "agent")).expanduser()
sessions_dir = Path(os.environ.get("PI_AGENT_SESSIONS_DIR", agent_home / "sessions")).expanduser()
models_file = Path(os.environ.get("PI_AGENT_MODELS_FILE", agent_home / "models.json")).expanduser()

# Read models.json to keep the helper tied to the same local Pi Agent config
# surface, but never publish model/provider names or credentials. Session usage
# rows already carry reduced cost/tokens, so models are not needed for pricing.
# NOTE: This creates a potential gap if future Pi Agent rows contain tokens but
# not cost, as models.json pricing config would be ignored. Currently safe
# because session rows include usage.cost; consider using models.json for
# pricing if session data becomes incomplete.
try:
    if models_file.is_file():
        json.loads(models_file.read_text(encoding="utf-8"))
except Exception:
    eprint("models.json parse skipped")

# Warn if models.json exists (contains pricing info) but we're not using it
if models_file.is_file():
    try:
        models_data = json.loads(models_file.read_text(encoding="utf-8"))
        # Check for common pricing fields that indicate this file has pricing config
        if any(key in models_data for key in ['pricing', 'models', 'providers']):
            eprint("note: models.json contains pricing config but session data is used for spend calculation")
    except Exception:
        pass

if not sessions_dir.is_dir():
    eprint(f"sessions dir absent: {sessions_dir}")
    sys.exit(3)

today = dt.datetime.now().astimezone().date()
dates = [today - dt.timedelta(days=i) for i in range(29, -1, -1)]
window = {d: {"dollars": 0.0, "tokens": 0} for d in dates}
start, end = dates[0], dates[-1]

derived_window = {target: {d: 0 for d in dates} for _, target in DERIVED_PREFIXES}

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
                target = derived_target(row_provider(row))
                if target is not None and day in derived_window[target]:
                    for usage in usage_objects(row):
                        derived_window[target][day] += usage_tokens(usage)

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
tok_hist = []
max_spend = 0
max_tokens = 0
prior_max_spend = 0
prior_max_tokens = 0
latest_spend = 0
latest_tokens = 0
any_usage = False
for i, day in enumerate(dates):
    vals = window[day]
    cents = int(round(vals["dollars"] * 100))
    tokens = int(round(vals["tokens"]))
    hist.append(cents)
    tok_hist.append(tokens)
    max_spend = max(max_spend, cents)
    max_tokens = max(max_tokens, tokens)
    if i < len(dates) - 1:
        prior_max_spend = max(prior_max_spend, cents)
        prior_max_tokens = max(prior_max_tokens, tokens)
    any_usage = any_usage or cents > 0 or tokens > 0
    latest_spend, latest_tokens = cents, tokens

if not any_usage:
    eprint(f"no usable Pi usage in last 30 days (files={files}, rows={rows})")
    sys.exit(3)

# Session % compares today against the peak single day before today so a new
# record can read above 100%. ps/pt still carry the overall 30-day max.
if prior_max_spend > 0:
    pct = round((latest_spend * 100.0) / prior_max_spend, 1)
elif prior_max_tokens > 0:
    pct = round((latest_tokens * 100.0) / prior_max_tokens, 1)
elif max_spend > 0:
    pct = 100.0
elif max_tokens > 0:
    pct = 100.0
else:
    pct = 0.0
pct = max(0.0, pct)

# Only fields the target card actually renders (see header): moonshot draws the
# balance card (tt + ht), qwencloud the standard card (tt + tm).
DERIVED_FIELDS = {"moonshot": ("tt", "ht"), "qwencloud": ("tt", "tm")}

derived = []
for target, by_day in derived_window.items():
    series = [int(by_day[d]) for d in dates]
    if not any(series):
        continue
    fields = DERIVED_FIELDS.get(target, ("tt",))
    entry = {"id": target, "tt": series[-1]}
    if "ht" in fields:
        entry["ht"] = series
    if "tm" in fields:
        entry["tm"] = sum(series)
    derived.append(entry)

provider = {
    "id": "pi",
    "ok": True,
    "p": pct,
    "pi": {"ts": latest_spend, "tt": latest_tokens, "ps": max_spend, "pt": max_tokens, "h": hist, "ht": tok_hist},
}
if derived:
    provider["derived"] = derived
print(json.dumps(provider, separators=(",", ":")))
eprint(
    f"reduced {used_rows} usage rows from {files} files: "
    f"today={latest_spend}c/{latest_tokens}tok max={max_spend}c/{max_tokens}tok hist={len(hist)}d"
)
for e in derived:
    eprint(f"derived {e['id']}: today={e['tt']}tok 30d={sum(derived_window[e['id']].values())}tok "
           f"(Pi-visible traffic only — undercounts other clients)")
PY
