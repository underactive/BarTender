#!/bin/zsh
# cursor-stats.sh — paginate Cursor dashboard usage events and emit a compact
# provider fragment merged by codexbar-publish.sh (in-place cu patch).
#
#   cursor-stats.sh             # emits {"id":"cursor","ok":true,"cu":{...}}
#   cursor-stats.sh --check     # auth probe only (no publish)
#   cursor-stats.sh --debug     # verbose stderr (no secrets)
#   cursor-stats.sh --help
#
# Reads:
#   Keychain  service=codexbar-toy  account=cursor-session
#             (WorkosCursorSessionToken value OR full Cookie: header from DevTools)
#   ~/.config/codexbar-toy/cursor-history.json   rolling 30-day token history
#
# Output (privacy-reduced; never logs the session token):
#   {"id":"cursor","ok":true,"cu":{"tk":12345,"mxt":89012,"ht":[...]}}
#
# Field units:
#   cu.tk   tokens today (Mac-local calendar day)
#   cu.mxt  30-day max daily tokens
#   cu.ht[] daily token totals, oldest -> newest, up to 31 chart points
#
# Env overrides (testability):
#   CURSOR_HISTORY       default: ~/.config/codexbar-toy/cursor-history.json
#   CURSOR_KC_SERVICE    default: codexbar-toy
#   CURSOR_KC_ACCOUNT    default: cursor-session
#   CURSOR_API_URL       default: https://cursor.com/api/dashboard/get-filtered-usage-events
#   PYTHON3              Python interpreter (default: command -v python3)
#
# Fail-soft: exit 3 when no credential and no history; publisher skips cu merge.
set -u

help() { awk 'NR>=2 && /^#/{sub(/^# ?/,"");print;next} NR>=2{exit}' "$0"; }
case "${1:-}" in
  "") ;;
  -h|--help) help; exit 0 ;;
  --debug) export CURSOR_DEBUG=1 ;;
  --check)
    export CURSOR_DEBUG=1
    export CURSOR_CHECK_ONLY=1
    ;;
  *) print -r -- "unknown argument: $1 (try --help)" >&2; exit 2 ;;
esac

PY="${PYTHON3:-$(command -v python3 2>/dev/null || true)}"
[[ -n "$PY" && -x "$PY" ]] || { print -r -- "cursor-stats: python3 not found" >&2; exit 2; }

# Heredoc-piped Python has no __file__, so hand it this script's dir to import
# scripts/lib/_stats_history.py (${0:A:h} = bin dir in zsh).
export CBTOY_SCRIPT_DIR="${0:A:h}/lib"
"$PY" <<'PY'
import datetime as dt
import json
import os
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, os.environ.get("CBTOY_SCRIPT_DIR", "."))
import _stats_history as _hist  # shared rolling-history helpers (Fowler audit #8)


def eprint(msg: str) -> None:
    print(f"cursor-stats: {msg}", file=sys.stderr)


def debug(msg: str) -> None:
    if os.environ.get("CURSOR_DEBUG"):
        eprint(f"debug: {msg}")


# Names that authenticate cursor.com dashboard APIs (CodexBar CursorCookieImporter).
SESSION_COOKIE_NAMES = frozenset(
    {
        "WorkosCursorSessionToken",
        "__Secure-next-auth.session-token",
        "next-auth.session-token",
        "wos-session",
        "__Secure-wos-session",
        "authjs.session-token",
        "__Secure-authjs.session-token",
    }
)


home = Path.home()
hist_file = Path(
    os.environ.get("CURSOR_HISTORY", home / ".config" / "codexbar-toy" / "cursor-history.json")
).expanduser()
kc_service = os.environ.get("CURSOR_KC_SERVICE", "codexbar-toy")
kc_account = os.environ.get("CURSOR_KC_ACCOUNT", "cursor-session")
api_url = os.environ.get(
    "CURSOR_API_URL",
    "https://cursor.com/api/dashboard/get-filtered-usage-events",
)


def normalize_cookie_header(raw: str) -> str | None:
    """Accept token-only, name=value, or full Cookie: / curl -H paste."""
    import re

    value = raw.strip()
    if not value:
        return None

    for pat in (
        r"(?i)-H\s*'Cookie:\s*([^']+)'",
        r'(?i)-H\s*"Cookie:\s*([^"]+)"',
        r"(?i)\bcookie:\s*'([^']+)'",
        r'(?i)\bcookie:\s*"([^"]+)"',
        r"(?i)\bcookie:\s*([^\r\n]+)",
    ):
        m = re.search(pat, value)
        if m:
            value = m.group(1).strip()
            break

    if value.lower().startswith("cookie:"):
        value = value[7:].strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
        value = value[1:-1].strip()
    if not value:
        return None

    # Bare session token (no name=value pairs) → wrap as Workos cookie.
    if "=" not in value:
        return f"WorkosCursorSessionToken={value}"
    return filter_cookie_header(value)


def filter_cookie_header(header: str) -> str:
    """Drop unrelated tracking cookies; keep session/auth pairs for cursor.com."""
    pairs: list[tuple[str, str]] = []
    for part in header.split(";"):
        part = part.strip()
        if not part or "=" not in part:
            continue
        name, val = part.split("=", 1)
        name = name.strip()
        val = val.strip()
        if not name:
            continue
        pairs.append((name, val))

    if not pairs:
        return header

    session_pairs = [(n, v) for n, v in pairs if n in SESSION_COOKIE_NAMES]
    if session_pairs:
        chosen = session_pairs
        debug(f"using {len(chosen)} session cookie(s): {', '.join(n for n, _ in chosen)}")
    else:
        chosen = pairs
        debug(f"no known session cookie names; sending all {len(chosen)} pair(s)")

    return "; ".join(f"{n}={v}" for n, v in chosen)


def get_cookie_header() -> str | None:
    try:
        out = subprocess.check_output(
            [
                "security",
                "find-generic-password",
                "-s",
                kc_service,
                "-a",
                kc_account,
                "-w",
            ],
            stderr=subprocess.DEVNULL,
            text=True,
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None
    return normalize_cookie_header(out)


def api_headers(cookie_header: str) -> dict[str, str]:
    # cursor.com dashboard APIs expect browser-like CSRF headers (see CodexBar / PixelAgents).
    return {
        "Content-Type": "application/json",
        "Accept": "application/json",
        "Cookie": cookie_header,
        "Origin": "https://cursor.com",
        "Referer": "https://cursor.com/dashboard?tab=usage",
        "User-Agent": (
            "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
            "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"
        ),
    }


def number(v):
    if v is None:
        return None
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def usage_tokens(obj: dict) -> int:
    if not isinstance(obj, dict):
        return 0
    for key in ("totalTokens", "tokenCount", "tokens"):
        n = number(obj.get(key))
        if n is not None:
            return int(round(n))
    parts = [
        "input",
        "output",
        "inputTokens",
        "outputTokens",
        "cacheRead",
        "cacheReadTokens",
        "cacheWrite",
        "cacheWriteTokens",
        "cacheCreate",
        "cached",
        "cachedInputTokens",
        "cacheHit",
        "cacheMiss",
    ]
    vals = [number(obj.get(k)) for k in parts]
    s = sum(v for v in vals if v is not None)
    return int(round(s)) if any(v is not None for v in vals) else 0


def extract_events(payload) -> list | None:
    if isinstance(payload, list):
        return payload
    if not isinstance(payload, dict):
        return None
    for key in (
        "usageEventsDisplay",
        "events",
        "usageEvents",
        "data",
        "items",
        "results",
    ):
        val = payload.get(key)
        if isinstance(val, list):
            return val
        if isinstance(val, dict):
            for key2 in ("events", "usageEvents", "usageEventsDisplay", "items", "data"):
                nested = val.get(key2)
                if isinstance(nested, list):
                    return nested
    return None


def event_day(ev: dict) -> str | None:
    for key in (
        "timestamp",
        "createdAt",
        "created_at",
        "eventTime",
        "eventAt",
        "time",
        "date",
        "startTime",
    ):
        v = ev.get(key)
        if v is None:
            continue
        try:
            if isinstance(v, (int, float)) or (isinstance(v, str) and v.isdigit()):
                ms = int(v)
                if ms < 1_000_000_000_000:
                    ms *= 1000
                return dt.datetime.fromtimestamp(ms / 1000).astimezone().date().isoformat()
            if isinstance(v, str):
                s = v.replace("Z", "+00:00")
                parsed = dt.datetime.fromisoformat(s)
                if parsed.tzinfo is None:
                    parsed = parsed.replace(tzinfo=dt.timezone.utc)
                return parsed.astimezone().date().isoformat()
        except (ValueError, OSError, OverflowError):
            continue
    return None


def extract_event_tokens(ev: dict) -> int:
    for key in ("usage", "tokenUsage", "tokens"):
        if isinstance(ev.get(key), dict):
            t = usage_tokens(ev[key])
            if t > 0:
                return t
    for key in ("totalTokens", "tokenCount", "tokens"):
        n = number(ev.get(key))
        if n is not None:
            return int(round(n))
    return 0


def probe_session(cookie_header: str) -> bool:
    """Quick auth check before paginating usage events."""
    req = urllib.request.Request(
        "https://cursor.com/api/auth/me",
        headers=api_headers(cookie_header),
        method="GET",
    )
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            raw = resp.read().decode("utf-8", errors="replace")
            if os.environ.get("CURSOR_DEBUG"):
                try:
                    me = json.loads(raw)
                    email = me.get("email") if isinstance(me, dict) else None
                    debug(f"/api/auth/me ok email={email or '?'}")
                except json.JSONDecodeError:
                    debug("/api/auth/me ok (non-json body)")
            return 200 <= resp.status < 300
    except urllib.error.HTTPError as e:
        if e.code in (401, 403):
            eprint(f"session rejected (HTTP {e.code} on /api/auth/me)")
            eprint(
                "re-run: ./scripts/codexbar-publish.sh --set-cursor-session "
                "and paste the full Cookie header from DevTools → Network "
                "(any cursor.com/api request), not just one cookie value"
            )
        else:
            eprint(f"session probe HTTP {e.code}")
        return False
    except (urllib.error.URLError, TimeoutError) as e:
        eprint(f"session probe failed: {e}")
        return False


def fetch_events(cookie_header: str, start_ms: int, end_ms: int) -> tuple[dict[str, int], bool]:
    daily: dict[str, int] = {}
    page = 1
    events_seen = 0
    while True:
        body = json.dumps(
            {
                "teamId": 0,
                "startDate": start_ms,
                "endDate": end_ms,
                "page": page,
                "pageSize": 100,
            }
        ).encode("utf-8")
        req = urllib.request.Request(
            api_url,
            data=body,
            headers=api_headers(cookie_header),
            method="POST",
        )
        try:
            with urllib.request.urlopen(req, timeout=25) as resp:
                raw = resp.read().decode("utf-8", errors="replace")
        except urllib.error.HTTPError as e:
            eprint(f"API HTTP {e.code} page {page}")
            if e.code in (401, 403) and page == 1:
                eprint(
                    "usage API rejected session — paste full Cookie header via "
                    "--set-cursor-session (see README)"
                )
            return daily, False
        except urllib.error.URLError as e:
            eprint(f"API network error page {page}: {e.reason}")
            return daily, False
        except TimeoutError:
            eprint(f"API timeout page {page}")
            return daily, False

        try:
            payload = json.loads(raw)
        except json.JSONDecodeError:
            eprint(f"API JSON parse fail page {page}")
            return daily, False

        if page == 1 and os.environ.get("CURSOR_DEBUG") and isinstance(payload, dict):
            keys = sorted(payload.keys())
            debug(f"page 1 top-level keys: {keys}")

        events = extract_events(payload)
        if not events:
            if page == 1:
                eprint("usage API returned no event list (empty or unrecognized shape)")
                if os.environ.get("CURSOR_DEBUG") and isinstance(payload, dict):
                    debug(f"payload preview: {json.dumps(payload)[:400]}")
            break

        if len(events) == 0:
            break

        page_tokens = 0
        for ev in events:
            if not isinstance(ev, dict):
                continue
            day = event_day(ev)
            if not day:
                continue
            tok = extract_event_tokens(ev)
            if tok <= 0:
                continue
            daily[day] = daily.get(day, 0) + tok
            page_tokens += tok

        events_seen += len(events)
        debug(f"page {page}: {len(events)} events, {page_tokens} tokens bucketed")

        if len(events) < 100:
            break
        if isinstance(payload, dict):
            total_n = number(payload.get("totalUsageEventsCount"))
            if total_n is not None and events_seen >= int(total_n):
                break
        page += 1

    return daily, True


def load_history() -> dict:
    # Shared corrupt-safe read, then coerce each entry to {"tk": int} since
    # cursor stores only token counts (drop malformed keys/values).
    out = {}
    for k, v in _hist.load_history(hist_file).items():
        if not isinstance(k, str) or not isinstance(v, dict):
            continue
        tk = v.get("tk", 0)
        try:
            out[k] = {"tk": int(tk)}
        except (TypeError, ValueError):
            continue
    return out


def save_history(history: dict) -> None:
    _hist.save_history(hist_file, history)


today = dt.date.today()
today_str = today.isoformat()
start_day = today - dt.timedelta(days=29)
start_local = dt.datetime.combine(start_day, dt.time.min).astimezone()
end_local = dt.datetime.combine(today + dt.timedelta(days=1), dt.time.min).astimezone()
start_ms = int(start_local.timestamp() * 1000)
end_ms = int(end_local.timestamp() * 1000) - 1

history = load_history()
api_succeeded = False
fetched: dict[str, int] = {}

cookie_header = get_cookie_header()
probe_ok = False
if cookie_header:
    debug(f"Keychain cookie header: {len(cookie_header)} chars")
    probe_ok = probe_session(cookie_header)
    if probe_ok:
        fetched, api_succeeded = fetch_events(cookie_header, start_ms, end_ms)
    else:
        fetched, api_succeeded = {}, False
    if api_succeeded:
        for day, tk in fetched.items():
            history[day] = {"tk": int(tk)}
    elif probe_ok:
        eprint("usage API returned no token events in the last 30 days")
else:
    eprint("no Keychain session token (service=%s account=%s)" % (kc_service, kc_account))

# Prune to 30 calendar days (inclusive window ending today)
history = _hist.prune_history(history, 30)

save_history(history)

sorted_asc = sorted(k for k in history.keys() if k <= today_str)
tok_hist = [history[d].get("tk", 0) for d in sorted_asc]
max_tok = max(tok_hist) if tok_hist else 0
today_tk = history.get(today_str, {}).get("tk", 0)

has_history = any(v.get("tk", 0) > 0 for v in history.values())
# LM-style: cached history OR a live authenticated API read (zeros are valid).
ok = has_history or (bool(cookie_header) and probe_ok and api_succeeded)

if os.environ.get("CURSOR_CHECK_ONLY"):
    print(
        json.dumps(
            {
                "keychain": bool(cookie_header),
                "probe": probe_ok,
                "api": api_succeeded,
                "history_days": len(history),
                "today_tk": today_tk,
                "ok": ok,
            },
            separators=(",", ":"),
        )
    )
    sys.exit(0 if ok else 3)

if not ok:
    eprint("no usable Cursor token data")
    print(json.dumps({"id": "cursor", "ok": False}, separators=(",", ":")))
    sys.exit(3)

provider = {
    "id": "cursor",
    "ok": True,
    "cu": {
        "tk": int(today_tk),
        "mxt": int(max_tok),
        "ht": tok_hist[-31:],
        "sess": bool(cookie_header) and probe_ok,
    },
}
print(json.dumps(provider, separators=(",", ":")))
eprint(
    f"today={today_tk}tok mxt={max_tok}tok hist={len(tok_hist)}d "
    f"api={'ok' if api_succeeded else 'skip'}"
)
PY
