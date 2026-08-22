#!/bin/zsh
# openrouter-stats.sh — pull OpenRouter token usage from the analytics API and
# emit a compact provider fragment consumed by codexbar-publish.sh (in-place
# `cost` patch adding tt/ht).
#
#   openrouter-stats.sh             # emits {"id":"openrouter","ok":true,"cost":{...}}
#   openrouter-stats.sh --check     # auth / connectivity probe only
#   openrouter-stats.sh --help
#
# Source is POST /api/v1/analytics/query at HOUR granularity, not the more
# obvious GET /api/v1/activity. /activity hard-rejects the in-progress UTC day
# ("Date must be within the last 30 (completed) UTC days"), so before ~17:00
# local its newest bucket holds none of today's usage. Hourly buckets also let
# us re-bucket into LOCAL calendar days, matching the rollup convention the
# rest of the pipeline uses.
#
# Reads:
#   Keychain  service=codexbar-toy  account=openrouter-key
#             (an OpenRouter *management* key from
#              https://openrouter.ai/settings/management-keys — ordinary
#              inference keys get 403 despite sharing the sk-or-v1- prefix)
#
# Output:
#   {"id":"openrouter","ok":true,"cost":{"tt":...,"ht":[...]}}
#
# Fields:
#   cost.tt   tokens today, local calendar day (prompt + completion)
#   cost.ht[] daily token totals, oldest -> newest, 30 points, zero-filled
#
# The API serves the full 30-day window on every call, so unlike the cookie
# scrapers there is no local history cache to keep in sync.
#
# Env overrides (testability):
#   OPENROUTER_MANAGEMENT_KEY  inline key for testing (bypasses Keychain)
#   OPENROUTER_KC_SERVICE      default: codexbar-toy
#   OPENROUTER_KC_ACCOUNT      default: openrouter-key
#   OPENROUTER_API_BASE        default: https://openrouter.ai/api/v1
#   OPENROUTER_HIST_DAYS       default: 30 (hourly range is API-capped at 31d)
#   PYTHON3                    Python interpreter (default: command -v python3)
#
# Fail-soft: exit 3 when no credential or the API is unreachable; the publisher
# then leaves the OpenRouter provider's existing cost block untouched.
set -u

# Capture script path before any function call (zsh: $0 is fn name inside function)
_SELF="$0"
help() { awk 'NR>=2 && /^#/{sub(/^# ?/,"");print;next} NR>=2{exit}' "$_SELF"; }
case "${1:-}" in
  "") ;;
  -h|--help) help; exit 0 ;;
  --check) export OR_CHECK_ONLY=1 ;;
  *) print -r -- "unknown argument: $1 (try --help)" >&2; exit 2 ;;
esac

PY="${PYTHON3:-$(command -v python3 2>/dev/null || true)}"
[[ -n "$PY" && -x "$PY" ]] || { print -r -- "openrouter-stats: python3 not found" >&2; exit 2; }

"$PY" <<'PY'
import datetime as dt
import json
import os
import subprocess
import sys
import urllib.error
import urllib.request


def eprint(msg: str) -> None:
    print(f"openrouter-stats: {msg}", file=sys.stderr)


kc_service = os.environ.get("OPENROUTER_KC_SERVICE", "codexbar-toy")
kc_account = os.environ.get("OPENROUTER_KC_ACCOUNT", "openrouter-key")
api_base = os.environ.get("OPENROUTER_API_BASE", "https://openrouter.ai/api/v1").rstrip("/")
try:
    hist_days = max(1, min(30, int(os.environ.get("OPENROUTER_HIST_DAYS", "30"))))
except ValueError:
    hist_days = 30


def get_key() -> str | None:
    override = os.environ.get("OPENROUTER_MANAGEMENT_KEY")
    if override and override.strip():
        return override.strip()
    try:
        out = subprocess.check_output(
            ["security", "find-generic-password", "-s", kc_service, "-a", kc_account, "-w"],
            stderr=subprocess.DEVNULL,
            text=True,
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None
    value = out.strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
        value = value[1:-1].strip()
    return value or None


def fetch_hours(key: str, start: dt.datetime, end: dt.datetime) -> list | None:
    """POST the analytics query and return the raw hourly rows, or None."""
    body = json.dumps({
        "metrics": ["tokens_total"],
        "granularity": "hour",
        "time_range": {
            "start": start.astimezone(dt.timezone.utc).strftime("%Y-%m-%dT%H:00:00Z"),
            "end": end.astimezone(dt.timezone.utc).strftime("%Y-%m-%dT%H:00:00Z"),
        },
        "limit": 2000,
    }).encode()
    req = urllib.request.Request(
        f"{api_base}/analytics/query",
        data=body,
        headers={"Authorization": f"Bearer {key}", "Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            payload = json.load(resp)
    except urllib.error.HTTPError as e:
        detail = ""
        try:
            detail = (json.load(e).get("error") or {}).get("message", "")
        except Exception:
            pass
        eprint(f"API HTTP {e.code}{': ' + detail if detail else ''}")
        return None
    except (urllib.error.URLError, TimeoutError) as e:
        eprint(f"API unreachable: {e}")
        return None
    except json.JSONDecodeError:
        eprint("API response not JSON")
        return None
    # Response nests two deep: {"data": {"data": [rows], "metadata", "cachedAt"}}
    inner = payload.get("data")
    rows = inner.get("data") if isinstance(inner, dict) else inner
    if not isinstance(rows, list):
        eprint("API response shape unrecognized")
        return None
    return rows


def bucket_local(rows: list, today: dt.date) -> dict[dt.date, int]:
    """Fold UTC hour buckets into local calendar days."""
    daily: dict[dt.date, int] = {}
    for r in rows:
        if not isinstance(r, dict):
            continue
        stamp = r.get("date__hour")
        try:
            when = dt.datetime.strptime(stamp, "%Y-%m-%d %H:%M:%S")
        except (TypeError, ValueError):
            continue
        # .astimezone() resolves the correct local offset per instant, so days
        # spanning a DST change bucket correctly.
        day = when.replace(tzinfo=dt.timezone.utc).astimezone().date()
        if day > today:
            continue
        try:
            tokens = int(r.get("tokens_total") or 0)   # API returns these as strings
        except (TypeError, ValueError):
            continue
        daily[day] = daily.get(day, 0) + max(0, tokens)
    return daily


today = dt.datetime.now().astimezone().date()
start = dt.datetime.combine(today - dt.timedelta(days=hist_days - 1), dt.time.min).astimezone()
end = dt.datetime.now(dt.timezone.utc) + dt.timedelta(hours=1)

key = get_key()
rows = fetch_hours(key, start, end) if key else None
if not key:
    eprint("no management key in Keychain or OPENROUTER_MANAGEMENT_KEY")

if rows is None:
    if os.environ.get("OR_CHECK_ONLY"):
        print(json.dumps({"keychain": bool(key), "api": False, "ok": False}, separators=(",", ":")))
        sys.exit(3)
    print(json.dumps({"id": "openrouter", "ok": False}, separators=(",", ":")))
    sys.exit(3)

daily = bucket_local(rows, today)
series = [daily.get(today - dt.timedelta(days=hist_days - 1 - i), 0) for i in range(hist_days)]
today_tk = series[-1]

if os.environ.get("OR_CHECK_ONLY"):
    print(json.dumps({
        "keychain": bool(key),
        "api": True,
        "hour_rows": len(rows),
        "active_days": sum(1 for v in series if v > 0),
        "today_tk": today_tk,
        "ok": True,
    }, separators=(",", ":")))
    sys.exit(0)

print(json.dumps(
    {"id": "openrouter", "ok": True, "cost": {"tt": today_tk, "ht": series}},
    separators=(",", ":"),
))
eprint(f"today={today_tk}tok hist={len(series)}d max={max(series)}tok hours={len(rows)}")
PY
