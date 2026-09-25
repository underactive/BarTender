#!/bin/zsh
# opencodego-stats.sh — scrape OpenCode Go usage from opencode.ai tRPC API
# and emit a compact provider fragment consumed by codexbar-publish.sh (in-place
# oc patch). Workspace usage data is read via a Keychain-stored auth cookie.
#
#   opencodego-stats.sh             # emits {"id":"opencodego","ok":true,"oc":{...}}
#   opencodego-stats.sh --check     # auth / connectivity probe only
#   opencodego-stats.sh --debug     # print pagination diagnostics
#   opencodego-stats.sh --help
#
# The API is a tRPC v10 endpoint returning per-request usage records wrapped in
# JavaScript $R-variable serialization. A small static Python parser extracts
# the response without executing response content, then Python groups by date,
# sums tokens/cost, and persists a rolling 30-day history.
#
# Reads:
#   Keychain  service=codexbar-toy  account=opencodego-session
#             (auth cookie value, NOT the "Cookie:" prefix)
#   ~/.config/codexbar-toy/opencodego-history.json   rolling 30-day token history
#
# Output:
#   {"id":"opencodego","ok":true,"oc":{"tk":...,"ct":...,"mxt":...,"ht":[...],"fresh":true}}
#
# Fields:
#   oc.tk   tokens today (inputTokens + outputTokens)
#   oc.ct   cost today in cents (API raw ÷ COST_DIVISOR; default divisor=10000)
#   oc.mxt  30-day max daily tokens
#   oc.ht[] daily token totals, oldest -> newest, up to 31 chart points
#   oc.fresh true for a successful API fetch, false for cached history
#
# Env overrides (testability):
#   OPENCODE_GO_COOKIE       inline cookie for testing (bypasses Keychain)
#   OPENCODE_GO_HISTORY      default: ~/.config/codexbar-toy/opencodego-history.json
#   OPENCODE_GO_WORKSPACE    default: current OpenCode Go workspace
#   OPENCODE_GO_KC_SERVICE   default: codexbar-toy
#   OPENCODE_GO_KC_ACCOUNT   default: opencodego-session
#   OPENCODE_GO_COST_DIVISOR default: 10000 (API raw ÷ this = cents)
#   OPENCODE_GO_DUMP         optional response dump path, written 0600
#   OPENCODE_GO_DEBUG        print per-page pagination diagnostics
#   PYTHON3                  Python interpreter (default: command -v python3)
#
# Fail-soft: exit 3 when no credential and no history; publisher skips oc merge.
set -u

# Capture script path before any function call (zsh: $0 is fn name inside function)
_SELF="$0"
help() { awk 'NR>=2 && /^#/{sub(/^# ?/,"");print;next} NR>=2{exit}' "$_SELF"; }
case "${1:-}" in
  "") ;;
  -h|--help) help; exit 0 ;;
  --check) export OG_CHECK_ONLY=1 ;;
  --debug) export OPENCODE_GO_DEBUG=1 ;;
  *) print -r -- "unknown argument: $1 (try --help)" >&2; exit 2 ;;
esac

PY="${PYTHON3:-$(command -v python3 2>/dev/null || true)}"
[[ -n "$PY" && -x "$PY" ]] || { print -r -- "opencodego-stats: python3 not found" >&2; exit 2; }

export CBTOY_SCRIPT_DIR="${0:A:h}/lib"
"$PY" <<'PY'
import datetime as dt
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from urllib.parse import quote

sys.path.insert(0, os.environ.get("CBTOY_SCRIPT_DIR", "."))
import _stats_history as _hist
from _trpc_extract import extract_records


def eprint(msg: str) -> None:
    print(f"opencodego-stats: {msg}", file=sys.stderr)


home = Path.home()
hist_file = Path(
    os.environ.get("OPENCODE_GO_HISTORY", home / ".config" / "codexbar-toy" / "opencodego-history.json")
).expanduser()
kc_service = os.environ.get("OPENCODE_GO_KC_SERVICE", "codexbar-toy")
kc_account = os.environ.get("OPENCODE_GO_KC_ACCOUNT", "opencodego-session")
workspace = os.environ.get("OPENCODE_GO_WORKSPACE", "wrk_01KT1W5K2X3FPZCZVFYYJWBEW8")
try:
    cost_divisor = int(os.environ.get("OPENCODE_GO_COST_DIVISOR", "10000"))
except ValueError:
    cost_divisor = 10000
if cost_divisor <= 0:
    cost_divisor = 10000
dump_path = os.environ.get("OPENCODE_GO_DUMP")
debug = os.environ.get("OPENCODE_GO_DEBUG", "") not in ("", "0")

# Keep the endpoint and routing/header constants aligned with the captured
# browser request. Only workspace and page offset are variable.
TRPC_URL_TEMPLATE = (
    "https://opencode.ai/_server"
    "?id=bfd684bfc2e4eed05cd0b518f5e4eafd3f3376e3938abb9e536e7c03df831e5c"
    "&args=%7B%22t%22%3A%7B%22t%22%3A9%2C%22i%22%3A0%2C%22l%22%3A2%2C%22a%22%3A%5B"
    "%7B%22t%22%3A1%2C%22s%22%3A%22{workspace}%22%7D%2C"
    "%7B%22t%22%3A0%2C%22s%22%3A{offset}%7D%5D%2C%22o%22%3A0%7D%2C%22f%22%3A31%2C%22m%22%3A%5B%5D%7D"
)

PAGE_SIZE = 50
MAX_PAGES = 40

CURL_HEADERS = [
    "-H", "Accept: */*",
    "-H", "Accept-Language: en-US,en;q=0.9",
    "-H", "Referer: https://opencode.ai/workspace/{workspace}/usage",
    "-H", 'Sec-CH-UA: "Not/A)Brand";v="99", "Chromium";v="148"',
    "-H", "Sec-CH-UA-Mobile: ?0",
    "-H", 'Sec-CH-UA-Platform: "macOS"',
    "-H", "Sec-Fetch-Dest: empty",
    "-H", "Sec-Fetch-Mode: cors",
    "-H", "Sec-Fetch-Site: same-origin",
    "-H", "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/148.0.0.0 Safari/537.36",
    "-H", "X-Server-Id: bfd684bfc2e4eed05cd0b518f5e4eafd3f3376e3938abb9e536e7c03df831e5c",
    "-H", "X-Server-Instance: server-fn:3",
]


def get_cookie() -> str | None:
    """Read OpenCode Go auth cookie from Keychain or env override."""
    override = os.environ.get("OPENCODE_GO_COOKIE")
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


def dump_response(raw: str) -> None:
    if not dump_path:
        return
    try:
        flags = os.O_WRONLY | os.O_CREAT | os.O_TRUNC
        fd = os.open(dump_path, flags, 0o600)
        try:
            os.fchmod(fd, 0o600)
            with os.fdopen(fd, "w", encoding="utf-8") as output:
                output.write(raw)
            fd = -1
        finally:
            if fd >= 0:
                os.close(fd)
    except OSError:
        eprint("response dump unavailable")


def _write_cookie_config(cookie: str) -> Path | None:
    """Write a mode-0600 curl config so the cookie never enters argv."""
    if "\r" in cookie or "\n" in cookie:
        eprint("cookie contains an invalid newline")
        return None
    fd = -1
    path: str | None = None
    try:
        fd, path = tempfile.mkstemp(prefix="opencodego-curl-", suffix=".conf")
        os.fchmod(fd, 0o600)
        escaped = cookie.replace("\\", "\\\\").replace('"', '\\"')
        stream = os.fdopen(fd, "w", encoding="utf-8")
        fd = -1
        with stream as config:
            config.write(f'cookie = "auth={escaped}"\\n')
        return Path(path)
    except OSError:
        if fd >= 0:
            os.close(fd)
        if path:
            try:
                os.unlink(path)
            except OSError:
                pass
        eprint("temporary cookie config unavailable")
        return None


def _record_key(rec: dict) -> tuple:
    rid = rec.get("id")
    if rid not in (None, ""):
        return ("id", str(rid))
    # Some pages omit ids. Keep the fallback key limited to usage fields so
    # model names and any future metadata cannot affect deduplication.
    return (
        "record",
        str(rec.get("timeCreated", "")),
        str(rec.get("inputTokens", "")),
        str(rec.get("outputTokens", "")),
        str(rec.get("cost", "")),
    )


def _int_value(value: object) -> int:
    try:
        return int(value or 0)
    except (TypeError, ValueError, OverflowError):
        return 0


def _fetch_usage(cookie_config: Path) -> dict[str, dict[str, int]] | None:
    """Fetch pages, statically parse records, dedupe, and group by date."""
    seen_keys: set[tuple] = set()
    all_records: list[dict] = []
    pages_fetched = 0

    for page in range(MAX_PAGES):
        offset = page
        url = TRPC_URL_TEMPLATE.format(workspace=quote(workspace, safe=""), offset=offset)
        headers = [item.format(workspace=workspace) for item in CURL_HEADERS]
        curl_args = ["curl", "-sS", "--config", str(cookie_config), url] + headers
        try:
            proc = subprocess.Popen(curl_args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            raw, _err = proc.communicate(timeout=20)
        except subprocess.TimeoutExpired:
            proc.kill()
            eprint(f"request timed out at offset {offset}")
            return None
        except FileNotFoundError:
            eprint("curl not found")
            return None
        if proc.returncode != 0:
            eprint(f"request failed at offset {offset}")
            return None
        pages_fetched += 1
        if not raw:
            eprint(f"empty response at offset {offset}")
            break
        dump_response(raw)
        records, reason = extract_records(raw)
        if records is None:
            eprint(f"response parse failed at offset {offset}: {reason or 'invalid'}")
            return None
        if len(records) == 0:
            break
        new_records = 0
        for rec in records:
            key = _record_key(rec)
            if key in seen_keys:
                continue
            seen_keys.add(key)
            all_records.append(rec)
            new_records += 1
        if debug:
            eprint(f"offset={offset}: {len(records)} raw, {new_records} new")
        if new_records < PAGE_SIZE:
            break

    if not all_records:
        eprint("no usage records fetched from API")
        return None

    daily: dict[str, dict[str, int]] = {}
    for rec in all_records:
        timestamp = rec.get("timeCreated")
        if not isinstance(timestamp, str) or len(timestamp) < 10:
            continue
        date_str = timestamp[:10]
        try:
            dt.date.fromisoformat(date_str)
        except ValueError:
            continue
        total_tokens = _int_value(rec.get("inputTokens")) + _int_value(rec.get("outputTokens"))
        raw_cost = _int_value(rec.get("cost"))
        daily.setdefault(date_str, {"tk": 0, "ct": 0})
        daily[date_str]["tk"] += total_tokens
        daily[date_str]["ct"] += raw_cost

    if not daily:
        eprint("no records with valid dates")
        return None

    eprint(f"total: {len(all_records)} unique records from {pages_fetched} page(s), {len(daily)} day(s)")
    return daily


def fetch_usage(cookie: str) -> dict[str, dict[str, int]] | None:
    curl_cookie = cookie
    for prefix in ("auth=", "cookie:"):
        if curl_cookie.lower().startswith(prefix):
            curl_cookie = curl_cookie[len(prefix):].strip()
    cookie_config = _write_cookie_config(curl_cookie)
    if cookie_config is None:
        return None
    try:
        return _fetch_usage(cookie_config)
    finally:
        try:
            cookie_config.unlink()
        except OSError:
            pass


def load_history() -> dict:
    out = {}
    for key, value in _hist.load_history(hist_file).items():
        if not isinstance(key, str) or not isinstance(value, dict):
            continue
        try:
            out[key] = {"tk": int(value.get("tk", 0)), "ct": int(value.get("ct", 0))}
        except (TypeError, ValueError):
            continue
    return out


def save_history(history: dict) -> None:
    _hist.save_history(hist_file, history)


today = dt.date.today()
today_str = today.isoformat()
history = load_history()

api_succeeded = False
cookie = get_cookie()

if cookie:
    daily = fetch_usage(cookie)
    if daily is not None:
        api_succeeded = True
        for date_str, data in daily.items():
            if date_str > today_str:
                continue
            ct_cents = max(0, int(data["ct"] / cost_divisor))
            history[date_str] = {
                "tk": max(0, int(data["tk"])),
                "ct": ct_cents,
            }
    else:
        eprint("API unavailable; using cached history")
else:
    eprint("credential unavailable; using cached history")

history = _hist.prune_history(history, 30)
save_history(history)

sorted_asc = sorted(key for key in history.keys() if key <= today_str)
tok_hist = [history[day].get("tk", 0) for day in sorted_asc]
max_tok = max(tok_hist) if tok_hist else 0
today_tk = history.get(today_str, {}).get("tk", 0)
today_ct = history.get(today_str, {}).get("ct", 0)

has_data = any(value.get("tk", 0) > 0 for value in history.values())
ok = has_data or (bool(cookie) and api_succeeded)
fresh = api_succeeded
cached = bool(has_data and not fresh)

if os.environ.get("OG_CHECK_ONLY"):
    print(json.dumps({
        "keychain": bool(cookie),
        "api": api_succeeded,
        "fresh": fresh,
        "cached": cached,
        "history_days": len(history),
        "today_tk": today_tk,
        "today_ct": today_ct,
        "ok": ok,
    }, separators=(",", ":")))
    sys.exit(0 if ok else 3)

if not ok:
    eprint("no usable OpenCode Go data")
    print(json.dumps({"id": "opencodego", "ok": False}, separators=(",", ":")))
    sys.exit(3)

provider = {
    "id": "opencodego",
    "ok": True,
    "oc": {
        "tk": int(today_tk),
        "ct": int(today_ct),
        "mxt": int(max_tok),
        "ht": tok_hist[-31:],
        "fresh": fresh,
    },
}
print(json.dumps(provider, separators=(",", ":")))
eprint(
    f"today={today_tk}tok ct={today_ct}cents mxt={max_tok}tok "
    f"hist={len(tok_hist)}d api={'ok' if api_succeeded else 'skip'} "
    f"source={'fresh' if fresh else 'cache'} cost_div={cost_divisor}"
)
PY
