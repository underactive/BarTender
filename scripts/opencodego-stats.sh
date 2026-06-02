#!/bin/zsh
# opencodego-stats.sh — scrape OpenCode Go usage from opencode.ai tRPC API
# and emit a compact provider fragment consumed by codexbar-publish.sh (in-place
# oc patch). Workspace usage data is read via a Keychain-stored auth cookie.
#
#   opencodego-stats.sh             # emits {"id":"opencodego","ok":true,"oc":{...}}
#   opencodego-stats.sh --check     # auth / connectivity probe only
#   opencodego-stats.sh --help
#
# The API is a tRPC v10 endpoint returning per-request usage records wrapped in
# JavaScript $R-variable serialization. Node.js evaluates the response to
# extract structured JSON, then Python groups by date, sums tokens/cost, and
# persists a rolling 30-day history.
#
# Reads:
#   Keychain  service=codexbar-toy  account=opencodego-session
#             (auth cookie value, NOT the "Cookie:" prefix)
#   ~/.config/codexbar-toy/opencodego-history.json   rolling 30-day token history
#
# Output:
#   {"id":"opencodego","ok":true,"oc":{"tk":...,"ct":...,"mxt":...,"ht":[...]}}
#
# Fields:
#   oc.tk   tokens today (inputTokens + outputTokens)
#   oc.ct   cost today in cents (API raw ÷ COST_DIVISOR; default divisor=10000)
#   oc.mxt  30-day max daily tokens
#   oc.ht[] daily token totals, oldest -> newest, up to 31 chart points
#
# Env overrides (testability):
#   OPENCODE_GO_COOKIE       inline cookie for testing (bypasses Keychain)
#   OPENCODE_GO_HISTORY      default: ~/.config/codexbar-toy/opencodego-history.json
#   OPENCODE_GO_KC_SERVICE   default: codexbar-toy
#   OPENCODE_GO_KC_ACCOUNT   default: opencodego-session
#   OPENCODE_GO_COST_DIVISOR default: 10000 (API raw ÷ this = cents)
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
  *) print -r -- "unknown argument: $1 (try --help)" >&2; exit 2 ;;
esac

PY="${PYTHON3:-$(command -v python3 2>/dev/null || true)}"
[[ -n "$PY" && -x "$PY" ]] || { print -r -- "opencodego-stats: python3 not found" >&2; exit 2; }

export CBTOY_SCRIPT_DIR="${0:A:h}/lib"
"$PY" <<'PY'
import datetime as dt
import json
import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, os.environ.get("CBTOY_SCRIPT_DIR", "."))
import _stats_history as _hist


def eprint(msg: str) -> None:
    print(f"opencodego-stats: {msg}", file=sys.stderr)


home = Path.home()
hist_file = Path(
    os.environ.get("OPENCODE_GO_HISTORY", home / ".config" / "codexbar-toy" / "opencodego-history.json")
).expanduser()
kc_service = os.environ.get("OPENCODE_GO_KC_SERVICE", "codexbar-toy")
kc_account = os.environ.get("OPENCODE_GO_KC_ACCOUNT", "opencodego-session")
cost_divisor = int(os.environ.get("OPENCODE_GO_COST_DIVISOR", "10000"))

# tRPC endpoint: encodes workspace ID + offset args in a server-procedure call.
# The `id` and `x-server-id` / `x-server-instance` are the procedure routing;
# `args` specifies the tRPC query parameters (type 9, workspace ID, offset 0).
# The {offset} placeholder is replaced with the page offset (0, 50, ...).
TRPC_URL_TEMPLATE = (
    "https://opencode.ai/_server"
    "?id=bfd684bfc2e4eed05cd0b518f5e4eafd3f3376e3938abb9e536e7c03df831e5c"
    "&args=%7B%22t%22%3A%7B%22t%22%3A9%2C%22i%22%3A0%2C%22l%22%3A2%2C%22a%22%3A%5B"
    "%7B%22t%22%3A1%2C%22s%22%3A%22wrk_01KT1W5K2X3FPZCZVFYYJWBEW8%22%7D%2C"
    "%7B%22t%22%3A0%2C%22s%22%3A{offset}%7D%5D%2C%22o%22%3A0%7D%2C%22f%22%3A31%2C%22m%22%3A%5B%5D%7D"
)

CURL_HEADERS = [
    "-H", "Accept: */*",
    "-H", "Accept-Language: en-US,en;q=0.9",
    "-H", "Referer: https://opencode.ai/workspace/wrk_01KT1W5K2X3FPZCZVFYYJWBEW8/usage",
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
    # Accept bare token or full Cookie header.
    if value.lower().startswith("cookie:"):
        value = value[7:].strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
        value = value[1:-1].strip()
    return value if value else None


def _parse_trpc(raw: str) -> list | None:
    """Parse a tRPC $R-variable response into a list of usage records."""
    if raw.strip().startswith("<html"):
        eprint("API returned HTML (login page) -- auth cookie likely expired"); return None
    clean = raw.strip().rstrip("%")
    parts = clean.split(";", 2)
    js_code = parts[2] if len(parts) >= 3 else None
    if not js_code:
        m = re.search(r";(?:0x[0-9a-fA-F]+|\d+);(.+)$", clean)
        js_code = m.group(1) if m else None
    if not js_code:
        eprint("tRPC response does not match expected format"); return None

    node_script = """
const vm = require('vm');
const chunks = [];
process.stdin.on('data', c => chunks.push(c));
process.stdin.on('end', () => {
    const jsCode = Buffer.concat(chunks).toString('utf8');
    const r = {};
    r['server-fn:3'] = [];
    const ctx = { self: { $R: r }, $R: r };
    try { vm.runInNewContext(jsCode, ctx); }
    catch (e) { process.stderr.write('node-vm: ' + e.message + '\\n'); process.exit(1); }
    const data = ctx.self.$R['server-fn:3'];
    if (!data || !Array.isArray(data) || !data[0] || !Array.isArray(data[0])) {
        process.stderr.write('node-vm: bad data shape\\n'); process.exit(1); }
    process.stdout.write(JSON.stringify(data[0]));
});
"""
    try:
        proc = subprocess.Popen(["node", "-e", node_script],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        out, err = proc.communicate(input=js_code, timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill(); eprint("Node.js VM timed out"); return None
    except FileNotFoundError:
        eprint("node not found"); return None
    if proc.returncode != 0:
        eprint(f"Node.js VM failed (exit {proc.returncode})")
        if err: eprint(err.strip())
        return None
    try:
        return json.loads(out)
    except json.JSONDecodeError as e:
        eprint(f"Node.js output not JSON: {e}")
        return None


def fetch_usage(cookie: str) -> dict | None:
    """Fetch ALL pages of usage from the OpenCode tRPC API and group by date.

    The API paginates at ~50 records per page. Fetch pages 0, 50, ... until
    fewer than 50 records come back, combine all records (deduped by id),
    and sum tokens/cost per calendar day.

    Returns {date_str: {"tk": int, "ct": int}} or None on failure.
    """
    curl_cookie = cookie
    for prefix in ("auth=", "cookie:"):
        if curl_cookie.lower().startswith(prefix):
            curl_cookie = curl_cookie[len(prefix):].strip()

    seen_ids: set[str] = set()
    all_records: list[dict] = []
    page_size = 50
    max_pages = 40

    for page in range(max_pages):
        offset = page  # API uses page numbers directly
        url = TRPC_URL_TEMPLATE.format(offset=offset)
        curl_args = ["curl", "-sS", url, "-b", f"auth={curl_cookie}"] + CURL_HEADERS
        try:
            proc = subprocess.Popen(curl_args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            raw, err = proc.communicate(timeout=20)
        except subprocess.TimeoutExpired:
            proc.kill(); eprint(f"curl timed out at offset {offset}"); return None
        except FileNotFoundError:
            eprint("curl not found"); return None
        if proc.returncode != 0:
            eprint(f"curl exit {proc.returncode} at offset {offset}"); return None
        if not raw:
            eprint(f"empty response at offset {offset}"); break
        records = _parse_trpc(raw)
        if records is None or not isinstance(records, list) or len(records) == 0:
            break
        new_records = 0
        for rec in records:
            rid = rec.get("id", "")
            if rid and rid in seen_ids:
                continue
            if rid:
                seen_ids.add(rid)
            all_records.append(rec)
            new_records += 1
        eprint(f"offset={offset}: {len(records)} raw, {new_records} new")
        if new_records < page_size:
            break

    if not all_records:
        eprint("no usage records fetched from API")
        return None

    daily: dict[str, dict] = {}
    for rec in all_records:
        ts = rec.get("timeCreated", "")
        if not ts:
            continue
        date_str = ts[:10]
        total_tk = int(rec.get("inputTokens", 0) or 0) + int(rec.get("outputTokens", 0) or 0)
        raw_cost = int(rec.get("cost", 0) or 0)
        daily.setdefault(date_str, {"tk": 0, "ct": 0})
        daily[date_str]["tk"] += total_tk
        daily[date_str]["ct"] += raw_cost

    if not daily:
        eprint("no records with valid dates")
        return None

    eprint(f"total: {len(all_records)} unique records from {page + 1} page(s), {len(daily)} day(s)")
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

api_succeeded = False
cookie = get_cookie()

if cookie:
    daily = fetch_usage(cookie)
    if daily is not None:
        api_succeeded = True
        # Merge API daily data into history (API data takes precedence)
        for date_str, data in daily.items():
            if date_str > today_str:
                continue
            # Convert API raw cost to cents using the configurable divisor
            ct_cents = max(0, int(data["ct"] / cost_divisor))
            history[date_str] = {
                "tk": max(0, int(data["tk"])),
                "ct": ct_cents,
            }
    else:
        eprint("API call failed — using cached history")
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
ok = has_data or (bool(cookie) and api_succeeded)

if os.environ.get("OG_CHECK_ONLY"):
    print(json.dumps({
        "keychain": bool(cookie),
        "api": api_succeeded,
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
    },
}
print(json.dumps(provider, separators=(",", ":")))
eprint(
    f"today={today_tk}tok ct={today_ct}cents mxt={max_tok}tok "
    f"hist={len(tok_hist)}d api={'ok' if api_succeeded else 'skip'} "
    f"cost_div={cost_divisor}"
)
PY
