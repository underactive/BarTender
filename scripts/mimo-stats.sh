#!/bin/zsh
# mimo-stats.sh — scrape Xiaomi MiMo usage from platform.xiaomimimo.com API
# and emit a compact provider fragment consumed by codexbar-publish.sh (in-place
# mo patch). Usage data is read via a Keychain-stored auth cookie.
#
#   mimo-stats.sh             # emits {"id":"mimo","ok":true,"mo":{...}}
#   mimo-stats.sh --cookie "userId=...; api-platform_ph=..."  # inline cookie (bypasses Keychain)
#   mimo-stats.sh --check     # auth / connectivity probe only
#   mimo-stats.sh --help
#
# Cookie: normally read from Keychain (set via codexbar-publish.sh --set-mimo-cookie).
# For one-shot runs, pass --cookie with the raw Cookie header from DevTools.
#
# Uses the summary endpoint (GET /api/v1/usage) for today's token and cost
# totals, plus the detail/list endpoint (POST) for per-date history. Both
# require cookie auth. Falls back to accumulating today's data into the history
# file over daily runs when the detail/list endpoint is unreachable.
#
# Reads:
#   Keychain  service=codexbar-toy  account=mimo-session
#             (raw Cookie string from browser DevTools)
#   ~/.config/codexbar-toy/mimo-history.json   rolling 30-day token history
#
# Output:
#   {"id":"mimo","ok":true,"mo":{"tk":...,"ct":...,"mxt":...,"ht":[...]}}
#
# Fields:
#   mo.tk    tokens today (totalToken from /api/v1/usage)
#   mo.ct    cost today in cents (USD totalCost × 100)
#   mo.mxt   30-day max daily tokens
#   mo.ht[]  daily token totals, oldest -> newest, up to 31 chart points
#
# Env overrides (testability):
#   MIMO_COOKIE              inline cookie for testing (bypasses Keychain)
#   MIMO_HISTORY             default: ~/.config/codexbar-toy/mimo-history.json
#   MIMO_KC_SERVICE          default: codexbar-toy
#   MIMO_KC_ACCOUNT          default: mimo-session
#   PYTHON3                  Python interpreter (default: command -v python3)
#
# Fail-soft: exit 3 when no credential and no history; publisher skips mo merge.
set -u

_SELF="$0"
help() { awk 'NR>=2 && /^#/{sub(/^# ?/,"");print;next} NR>=2{exit}' "$_SELF"; }
case "${1:-}" in
  "") ;;
  -h|--help) help; exit 0 ;;
  --check) export MIMO_CHECK_ONLY=1 ;;
  --cookie) MIMO_COOKIE="${2:-}"
    [[ -n "$MIMO_COOKIE" ]] || { print -r -- "missing cookie value after --cookie" >&2; exit 2; }
    export MIMO_COOKIE
    shift ;;
  *) print -r -- "unknown argument: $1 (try --help)" >&2; exit 2 ;;
esac

PY="${PYTHON3:-$(command -v python3 2>/dev/null || true)}"
[[ -n "$PY" && -x "$PY" ]] || { print -r -- "mimo-stats: python3 not found" >&2; exit 2; }

export CBTOY_SCRIPT_DIR="${0:A:h}/lib"
"$PY" <<'PY'
import datetime as dt
import json
import os
import subprocess
import sys
import urllib.request
import urllib.error
from pathlib import Path

sys.path.insert(0, os.environ.get("CBTOY_SCRIPT_DIR", "."))
import _stats_history as _hist


def eprint(msg: str) -> None:
    print(f"mimo-stats: {msg}", file=sys.stderr)


home = Path.home()
hist_file = Path(
    os.environ.get("MIMO_HISTORY", home / ".config" / "codexbar-toy" / "mimo-history.json")
).expanduser()
kc_service = os.environ.get("MIMO_KC_SERVICE", "codexbar-toy")
kc_account = os.environ.get("MIMO_KC_ACCOUNT", "mimo-session")

API_BASE = "https://platform.xiaomimimo.com/api/v1"
BASE_HEADERS = {
    "Accept": "application/json, text/plain, */*",
    "Accept-Language": "en-US,en;q=0.9",
    "Origin": "https://platform.xiaomimimo.com",
    "Referer": "https://platform.xiaomimimo.com/console/usage",
    "User-Agent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/148.0.0.0 Safari/537.36",
}


def get_cookie() -> str | None:
    """Read MiMo auth cookie from Keychain or env override."""
    override = os.environ.get("MIMO_COOKIE")
    if override:
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
    if not value:
        return None
    if value.lower().startswith("cookie:"):
        value = value[7:].strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
        value = value[1:-1].strip()
    return value if value else None


def _request(url: str, method: str = "GET", body: bytes | None = None, cookie: str = "") -> dict | None:
    """Make an authenticated API request. Returns parsed JSON dict or None."""
    headers = dict(BASE_HEADERS)
    headers["Cookie"] = cookie
    if body is not None:
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=body, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            raw = resp.read().decode("utf-8")
    except urllib.error.HTTPError as e:
        if e.code == 401:
            eprint("HTTP 401 — cookie expired or invalid")
        else:
            eprint(f"HTTP {e.code}")
        return None
    except Exception as e:
        eprint(f"request failed: {e}")
        return None
    try:
        return json.loads(raw)
    except json.JSONDecodeError:
        eprint("invalid JSON response")
        return None


def fetch_summary(cookie: str) -> tuple[int, int] | None:
    """Fetch today's token total and cost from the summary endpoint.
    Returns (totalToken, costCents) or None."""
    data = _request(f"{API_BASE}/usage", cookie=cookie)
    if not data or data.get("code") != 0:
        return None
    d = data.get("data", {})
    tok = int(d.get("tokenUsage", {}).get("totalToken", 0) or 0)
    cost_str = d.get("costUsage", {}).get("totalCost", "0")
    try:
        cost_cents = max(0, int(round(float(cost_str) * 100)))
    except (ValueError, TypeError):
        cost_cents = 0
    return (tok, cost_cents)


def fetch_detail_list(cookie: str) -> dict | None:
    """Fetch per-date detail records from the list endpoint.

    Calls POST /api/v1/usage/detail/list with {"year":YYYY,"month":M}
    body for the current month, plus previous months back to 30 days.
    The API validates the x-timeZone header.

    Returns dict of {date_str: {"tk": totalToken, "ct": costCents}} or None.
    """
    ph = ""
    for part in cookie.replace(" ", "").split(";"):
        if "=" not in part:
            continue
        k, v = part.split("=", 1)
        if k == "api-platform_ph":
            from urllib.parse import quote
            ph = quote(v.strip('"\''), safe="")
            break

    url = f"{API_BASE}/usage/detail/list"
    if ph:
        url += f"?api-platform_ph={ph}"

    all_records: list[dict] = []
    # Query current month and previous month (covers up to ~60 days)
    now = dt.date.today()
    months = [(now.year, now.month)]
    if now.month == 1:
        months.append((now.year - 1, 12))
    else:
        months.append((now.year, now.month - 1))

    for year, month in months:
        body = json.dumps({"year": year, "month": month}).encode("utf-8")
        # Need x-timeZone header
        headers = dict(BASE_HEADERS)
        headers["Cookie"] = cookie
        headers["Content-Type"] = "application/json"
        headers["x-timeZone"] = "America/Los_Angeles"

        req = urllib.request.Request(url, data=body, headers=headers, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=20) as resp:
                raw = resp.read().decode("utf-8")
        except urllib.error.HTTPError as e:
            if e.code == 401:
                eprint("HTTP 401 — cookie expired or invalid")
                return None
            eprint(f"HTTP {e.code} for {year}-{month:02d}")
            continue
        except Exception as e:
            eprint(f"request failed: {e}")
            continue

        try:
            data = json.loads(raw)
        except json.JSONDecodeError:
            continue

        if data.get("code") != 0:
            continue
        records = data.get("data", [])
        if records:
            all_records.extend(records)
            eprint(f"  {year}-{month:02d}: {len(records)} records")

    if not all_records:
        return None

    # Aggregate by date
    daily: dict[str, dict] = {}
    for rec in all_records:
        date_str = rec.get("date", "")
        if not date_str:
            continue
        total_tk = int(rec.get("totalToken", 0) or 0)
        cost_str = rec.get("consumedAmount", "0")
        try:
            cost_cents = max(0, int(round(float(cost_str) * 100)))
        except (ValueError, TypeError):
            cost_cents = 0

        if date_str not in daily:
            daily[date_str] = {"tk": 0, "ct": 0}
        daily[date_str]["tk"] += total_tk
        daily[date_str]["ct"] += cost_cents

    eprint(f"detail/list: {len(all_records)} records, {len(daily)} days")
    return daily


def load_history() -> dict:
    out = {}
    for k, v in _hist.load_history(hist_file).items():
        if not isinstance(k, str) or not isinstance(v, dict):
            continue
        try:
            out[k] = {"tk": int(v.get("tk", 0)), "ct": int(v.get("ct", 0))}
        except (TypeError, ValueError):
            continue
    return out


def save_history(history: dict) -> None:
    _hist.save_history(hist_file, history)


today = dt.date.today()
today_str = today.isoformat()
history = load_history()

cookie = get_cookie()
api_succeeded = False
provider_ok = False
balance_c = 0
gift_balance_c = 0

if cookie:
    # Try detail/list first (gives per-date history)
    detail_results = fetch_detail_list(cookie)
    if detail_results is not None:
        api_succeeded = True
        for date_str, data in detail_results.items():
            if date_str > today_str:
                continue
            history[date_str] = data
    else:
        # Fall back to summary endpoint for today's data only
        eprint("detail/list failed; trying summary endpoint")
        summary = fetch_summary(cookie)
        if summary is not None:
            api_succeeded = True
            tk, ct = summary
            history[today_str] = {"tk": tk, "ct": ct}
        else:
            eprint("API calls failed — using cached history")

    # Fetch balance (independent — succeeds or fails separately)
    bal_data = _request(f"{API_BASE}/balance", cookie=cookie)
    if bal_data and bal_data.get("code") == 0:
        bd = bal_data.get("data", {})
        try:
            balance_c = max(0, int(round(float(bd.get("cashBalance", "0")) * 100)))
            gift_balance_c = max(0, int(round(float(bd.get("giftBalance", "0")) * 100)))
            eprint(f"balance: \${balance_c/100:.2f} cash, \${gift_balance_c/100:.2f} gift")
        except (ValueError, TypeError):
            pass
else:
    eprint("no Keychain cookie — using cached history")

# Prune to 30 calendar days
history = _hist.prune_history(history, 30)
save_history(history)

sorted_asc = sorted(k for k in history.keys() if k <= today_str)
tok_hist = [history[d].get("tk", 0) for d in sorted_asc]
max_tok = max(tok_hist) if tok_hist else 0
today_tk = history.get(today_str, {}).get("tk", 0)
today_ct = history.get(today_str, {}).get("ct", 0)

has_data = any(v.get("tk", 0) > 0 for v in history.values())
provider_ok = has_data or (bool(cookie) and api_succeeded)

if os.environ.get("MIMO_CHECK_ONLY"):
    # Auth probe for codexbar-publish auto-refresh: fail when the live API
    # is unreachable so Playwright can refresh Keychain cookies. Do not treat
    # cached history as "ok" here — stale cache was blocking refresh.
    check_ok = api_succeeded
    print(json.dumps({
        "keychain": bool(cookie),
        "api": api_succeeded,
        "history_days": len(history),
        "today_tk": today_tk,
        "today_ct": today_ct,
        "ok": check_ok,
    }, separators=(",", ":")))
    sys.exit(0 if check_ok else 3)

if not provider_ok:
    eprint("no usable MiMo data")
    print(json.dumps({"id": "mimo", "ok": False}, separators=(",", ":")))
    sys.exit(3)

provider = {
    "id": "mimo",
    "ok": True,
    "mo": {
        "tk": int(today_tk),
        "ct": int(today_ct),
        "mxt": int(max_tok),
        "ht": tok_hist[-31:],
        "bl": balance_c,
        "gbl": gift_balance_c,
    },
}
print(json.dumps(provider, separators=(",", ":")))
eprint(
    f"today={today_tk}tok ct={today_ct}cents mxt={max_tok}tok "
    f"hist={len(tok_hist)}d api={'ok' if api_succeeded else 'skip'}"
)
PY
