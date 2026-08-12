#!/bin/zsh
# ramp-stats.sh — read Ramp Router (router.ramp.com) balance + usage and emit
# a compact provider object appended by codexbar-publish.sh (merge-ramp.js).
#
#   ramp-stats.sh             # emits {"id":"ramp","ok":true,"cost":{...}}
#   ramp-stats.sh --cookie "..."   # inline Cookie header (bypasses Keychain)
#   ramp-stats.sh --check     # auth / connectivity probe only
#   ramp-stats.sh --help
#
# Auth: the router.ramp.com session cookie (Keychain, set via
# codexbar-publish.sh --set-ramp-cookie) mints a short-lived JWT from
# GET /api/auth/token; the JWT then authorizes the dashboard APIs:
#   GET /client/billing           balance snapshot (remaining / total credits)
#   GET /client/usage/dashboard   30-day spend/token summary + per-day series
#
# Reads:
#   Keychain  service=codexbar-toy  account=ramp-session
#             (raw Cookie header from browser DevTools on router.ramp.com)
#   ~/.config/codexbar-toy/ramp-cache.json   last good API result (fail-soft)
#
# Output is privacy-reduced: aggregate cents/tokens only — no model names,
# request contents, API keys, or account identifiers. Uses the generic `cost`
# payload block (docs/generated/codexbar-payload.schema.json):
#   {"id":"ramp","ok":true,"cost":{"ct":9,"cm":9,"tt":25790,"tm":25790,
#                                  "cr":9991,"cl":10000,"h":[...]}}
#
# Field units:
#   cost.ct   spend today, cents
#   cost.cm   spend trailing 30 days, cents
#   cost.tt   tokens today
#   cost.tm   tokens trailing 30 days
#   cost.cr   balance remaining, cents
#   cost.cl   total credits, cents
#   cost.h[]  per-day spend cents, oldest -> newest, up to 31 points
#
# Env overrides (testability):
#   RAMP_COOKIE       inline cookie for testing (bypasses Keychain)
#   RAMP_CACHE        default: ~/.config/codexbar-toy/ramp-cache.json
#   RAMP_KC_SERVICE   default: codexbar-toy
#   RAMP_KC_ACCOUNT   default: ramp-session
#   RAMP_API_BASE     default: https://router.ramp.com
#   PYTHON3           Python interpreter (default: command -v python3)
#
# Fail-soft: exit 3 when no credential and no cache; the publisher logs the
# skip and publishes without Ramp. With a cache, an expired cookie re-emits
# the last good snapshot so the device tile degrades to stale, not absent.
set -u

_SELF="$0"
help() { awk 'NR>=2 && /^#/{sub(/^# ?/,"");print;next} NR>=2{exit}' "$_SELF"; }
case "${1:-}" in
  "") ;;
  -h|--help) help; exit 0 ;;
  --check) export RAMP_CHECK_ONLY=1 ;;
  --cookie) RAMP_COOKIE="${2:-}"
    [[ -n "$RAMP_COOKIE" ]] || { print -r -- "missing cookie value after --cookie" >&2; exit 2; }
    export RAMP_COOKIE
    shift ;;
  *) print -r -- "unknown argument: $1 (try --help)" >&2; exit 2 ;;
esac

PY="${PYTHON3:-$(command -v python3 2>/dev/null || true)}"
[[ -n "$PY" && -x "$PY" ]] || { print -r -- "ramp-stats: python3 not found" >&2; exit 2; }

"$PY" <<'PY'
import datetime as dt
import json
import os
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path


def eprint(msg: str) -> None:
    print(f"ramp-stats: {msg}", file=sys.stderr)


home = Path.home()
cache_file = Path(
    os.environ.get("RAMP_CACHE", home / ".config" / "codexbar-toy" / "ramp-cache.json")
).expanduser()
kc_service = os.environ.get("RAMP_KC_SERVICE", "codexbar-toy")
kc_account = os.environ.get("RAMP_KC_ACCOUNT", "ramp-session")
api_base = os.environ.get("RAMP_API_BASE", "https://router.ramp.com").rstrip("/")

BASE_HEADERS = {
    "Accept": "application/json, text/plain, */*",
    "Accept-Language": "en-US,en;q=0.9",
    "Origin": "https://router.ramp.com",
    "Referer": "https://router.ramp.com/dashboard",
    "User-Agent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/148.0.0.0 Safari/537.36",
}


def get_cookie() -> str | None:
    override = os.environ.get("RAMP_COOKIE")
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


def get_json(url: str, headers: dict) -> dict | None:
    req = urllib.request.Request(url, headers={**BASE_HEADERS, **headers})
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            raw = resp.read().decode("utf-8")
    except urllib.error.HTTPError as e:
        eprint("HTTP 401 — session cookie expired or invalid" if e.code == 401
               else f"HTTP {e.code} for {url.split('?')[0]}")
        return None
    except Exception as e:
        eprint(f"request failed: {e}")
        return None
    try:
        return json.loads(raw)
    except json.JSONDecodeError:
        eprint("invalid JSON response")
        return None


def mint_jwt(cookie: str) -> str | None:
    """Session cookie -> short-lived (~15 min) Bearer JWT for the client APIs."""
    data = get_json(f"{api_base}/api/auth/token", {"Cookie": cookie})
    tok = data.get("token") if isinstance(data, dict) else None
    return tok if isinstance(tok, str) and tok else None


def local_tz_name() -> str:
    try:
        p = os.path.realpath("/etc/localtime")
        if "/zoneinfo/" in p:
            return p.split("/zoneinfo/", 1)[1]
    except OSError:
        pass
    return "America/Los_Angeles"


def cents(v) -> int:
    try:
        return max(0, int(round(float(v) * 100)))
    except (TypeError, ValueError):
        return 0


def fetch_cost(jwt: str) -> dict | None:
    """Billing + 30-day usage -> generic `cost` block, or None on any failure."""
    hdr = {"Authorization": f"Bearer {jwt}"}

    billing = get_json(f"{api_base}/client/billing", hdr)
    if not isinstance(billing, dict):
        return None
    snap = billing.get("snapshot") or {}
    cr = cents(snap.get("remaining_credit_usd"))
    cl = cents(snap.get("total_credits_usd"))

    local = dt.datetime.now().astimezone()
    midnight = local.replace(hour=0, minute=0, second=0, microsecond=0)
    start = (midnight - dt.timedelta(days=30)).astimezone(dt.timezone.utc)
    end = (midnight + dt.timedelta(days=1)).astimezone(dt.timezone.utc)
    from urllib.parse import urlencode
    q = urlencode({
        "start_at": start.strftime("%Y-%m-%dT%H:%M:%S.000Z"),
        "end_at": end.strftime("%Y-%m-%dT%H:%M:%S.000Z"),
        "group_by": "model",
        "timezone": local_tz_name(),
    })
    usage = get_json(f"{api_base}/client/usage/dashboard?{q}", hdr)
    if not isinstance(usage, dict):
        return None
    summary = usage.get("summary") or {}

    # Aggregate the per-day-per-model series into local-date buckets.
    daily: dict[str, dict] = {}
    for e in usage.get("series") or []:
        try:
            when = dt.datetime.fromisoformat(
                str(e.get("period_start_at", "")).replace("Z", "+00:00"))
        except ValueError:
            continue
        day = when.astimezone(local.tzinfo).date().isoformat()
        b = daily.setdefault(day, {"tk": 0, "ct": 0})
        b["tk"] += int(e.get("total_tokens", 0) or 0)
        b["ct"] += cents(e.get("spend_usd"))

    today = local.date()
    tt = daily.get(today.isoformat(), {}).get("tk", 0)
    ct = daily.get(today.isoformat(), {}).get("ct", 0)
    h = [daily.get((today - dt.timedelta(days=n)).isoformat(), {}).get("ct", 0)
         for n in range(30, -1, -1)]

    return {
        "ct": ct,
        "cm": cents(summary.get("spend_usd")),
        "tt": tt,
        "tm": int(summary.get("total_tokens", 0) or 0),
        "cr": cr,
        "cl": cl,
        "h": h,
    }


def load_cache() -> dict | None:
    try:
        data = json.loads(cache_file.read_text())
    except (OSError, json.JSONDecodeError):
        return None
    return data.get("cost") if isinstance(data, dict) else None


def save_cache(cost: dict) -> None:
    try:
        cache_file.parent.mkdir(parents=True, exist_ok=True)
        cache_file.write_text(json.dumps(
            {"fetched_at": dt.datetime.now().astimezone().isoformat(), "cost": cost},
            separators=(",", ":")))
    except OSError as e:
        eprint(f"cache write failed: {e}")


cookie = get_cookie()
cost = None
api_ok = False
if cookie:
    jwt = mint_jwt(cookie)
    if jwt:
        cost = fetch_cost(jwt)
        api_ok = cost is not None
else:
    eprint("no Keychain cookie — run: codexbar-publish.sh --set-ramp-cookie")

if os.environ.get("RAMP_CHECK_ONLY"):
    print(json.dumps({"keychain": bool(cookie), "api": api_ok,
                      "ok": api_ok}, separators=(",", ":")))
    sys.exit(0 if api_ok else 3)

if api_ok:
    save_cache(cost)
else:
    cost = load_cache()
    if cost is None:
        eprint("no usable Ramp data (API failed, no cache)")
        print(json.dumps({"id": "ramp", "ok": False}, separators=(",", ":")))
        sys.exit(3)
    eprint("API failed — re-emitting cached snapshot")

print(json.dumps({"id": "ramp", "ok": True, "cost": cost}, separators=(",", ":")))
eprint(f"today={cost['tt']}tok/{cost['ct']}c 30d={cost['tm']}tok/{cost['cm']}c "
       f"bal={cost['cr']}c/{cost['cl']}c api={'ok' if api_ok else 'cache'}")
PY
