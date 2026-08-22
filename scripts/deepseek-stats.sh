#!/bin/zsh
# deepseek-stats.sh — pull DeepSeek per-day TOKEN and SPEND totals from the
# platform dashboard API and emit a compact provider fragment consumed by
# codexbar-publish.sh (in-place `cost` patch adding tt/ht/ct/cw).
#
#   deepseek-stats.sh             # emits {"id":"deepseek","ok":true,"cost":{...}}
#   deepseek-stats.sh --check     # auth / connectivity probe only
#   deepseek-stats.sh --help
#
# CodexBar's DeepSeek hook calls only api.deepseek.com/user/balance, which
# returns a balance and no usage — which is why the toy's DeepSeek card renders
# a balance with empty spend/token rows. These are the PRIVATE dashboard
# endpoints platform.deepseek.com's own console calls. They are undocumented
# and may change without notice.
#
#   GET {base}/usage/amount?month=<M>&year=<YYYY>   token counts
#   GET {base}/usage/cost?month=<M>&year=<YYYY>     money (currency-tagged)
#
# Both are MONTH-SCOPED and return the whole month, so a 30-day window needs
# the current month plus (unless the window starts this month) the previous
# one, stitched on the absolute `date` keys. Because the full window is
# re-derivable on every run there is NO local history cache here — the same
# property that makes openrouter-stats.sh cache-free, unlike the cursor /
# lmstudio / mimo scrapers that can only ever observe "today".
#
# Reads:
#   Keychain  service=codexbar-toy  account=deepseek-token
#             The platform `userToken` — a single opaque string, NOT a cookie
#             jar and NOT an sk-... API key (an API key cannot read the
#             dashboard; it gets code 40003). DevTools > Application >
#             Local Storage > https://platform.deepseek.com > key `userToken`.
#             Sent as `Authorization: Bearer <token>`.
#             That cell holds a JSON envelope, not a bare string
#             ({"value":"<64 chars>","__version":"0"}), so both the envelope
#             and the unwrapped token are accepted.
#
# Output:
#   {"id":"deepseek","ok":true,"cost":{"tt":..,"ht":[..],"ct":..,"cw":..}}
#
# Fields:
#   cost.tt   tokens today (see DAY BOUNDARY below)
#   cost.ht[] daily token totals, oldest -> newest, 30 points, zero-filled
#   cost.ct   spend today, integer cents
#   cost.cw   spend over the trailing 7 days incl. today, integer cents
#
# `cost.h` (daily spend history) is deliberately NOT published: the balance
# card renderer (ui_render_card.c render_cost_openrouter) charts tok_hist and
# never reads p->hist, so it would be ~250 B of payload that renders nothing.
# The device response buffer is a fixed size (fetch.c) shared by every
# provider, and overflow blanks the whole toy rather than one tile.
#
# DAY BOUNDARY (documented compromise): the API returns days PRE-BUCKETED as
# `YYYY-MM-DD` strings with no timezone, and offers no hourly granularity to
# re-bucket from. The rest of this pipeline rolls up on the Mac's LOCAL
# calendar day; here we can only match the API's day string against the local
# date. DeepSeek almost certainly buckets in Asia/Shanghai (UTC+8), so its
# "2026-08-21" covers roughly 2026-08-20 09:00 -> 2026-08-21 09:00 US/Pacific.
# We do NOT guess a shift — an unverified correction would misattribute usage
# more confidently than a documented offset does. DeepSeek's TODAY figure is
# therefore not the same 24 hours as the Claude/Codex/OpenRouter tiles beside
# it.
#
# The one correction we DO make is collapsing future-dated buckets into today
# (see fold_future). DeepSeek's timezone is ahead of US local time either way
# — UTC or UTC+8 — so during the local afternoon and evening today's usage is
# filed under tomorrow's date. Dropping it would blank the token row for up to
# ~15 hours after real activity, which is a worse lie than the offset itself.
#
# Tokens vs requests: each usage entry's `type` is one of
# PROMPT_CACHE_HIT_TOKEN / PROMPT_CACHE_MISS_TOKEN / RESPONSE_TOKEN / REQUEST.
# REQUEST is a request COUNT, not tokens — it is excluded from both sums. Any
# future non-REQUEST type is INCLUDED, on the assumption that a new token class
# should count.
#
# Currency: the cost endpoint tags each block USD or CNY. cost.ct/cw are
# defined as USD cents by the payload schema, so money is published ONLY when a
# USD block is present. Tokens are currency-agnostic and always published.
#
# Env overrides (testability):
#   DEEPSEEK_PLATFORM_TOKEN  inline token for testing (bypasses Keychain)
#   DEEPSEEK_USER_TOKEN      alias, matching CodexBar's env names
#   DEEPSEEK_KC_SERVICE      default: codexbar-toy
#   DEEPSEEK_KC_ACCOUNT      default: deepseek-token
#   DEEPSEEK_API_BASE        default: https://platform.deepseek.com/api/v0
#   DEEPSEEK_HIST_DAYS       default: 30
#   PYTHON3                  Python interpreter (default: command -v python3)
#
# Fail-soft: exit 3 when there is no credential, the API is unreachable, or
# auth is rejected; the publisher then leaves DeepSeek's existing cost block
# (the balance CodexBar scraped) untouched.
set -u

# Capture script path before any function call (zsh: $0 is fn name inside function)
_SELF="$0"
help() { awk 'NR>=2 && /^#/{sub(/^# ?/,"");print;next} NR>=2{exit}' "$_SELF"; }
case "${1:-}" in
  "") ;;
  -h|--help) help; exit 0 ;;
  --check) export DS_CHECK_ONLY=1 ;;
  *) print -r -- "unknown argument: $1 (try --help)" >&2; exit 2 ;;
esac

PY="${PYTHON3:-$(command -v python3 2>/dev/null || true)}"
[[ -n "$PY" && -x "$PY" ]] || { print -r -- "deepseek-stats: python3 not found" >&2; exit 2; }

"$PY" <<'PY'
import datetime as dt
import json
import os
import subprocess
import sys
import urllib.error
import urllib.request
from decimal import Decimal, InvalidOperation, ROUND_HALF_UP


def eprint(msg: str) -> None:
    print(f"deepseek-stats: {msg}", file=sys.stderr)


kc_service = os.environ.get("DEEPSEEK_KC_SERVICE", "codexbar-toy")
kc_account = os.environ.get("DEEPSEEK_KC_ACCOUNT", "deepseek-token")
api_base = os.environ.get("DEEPSEEK_API_BASE", "https://platform.deepseek.com/api/v0").rstrip("/")
try:
    hist_days = max(1, min(31, int(os.environ.get("DEEPSEEK_HIST_DAYS", "30"))))
except ValueError:
    hist_days = 30

# Auth rejection arrives as HTTP 200 with one of these in `code` or
# `data.biz_code` (deepseek-ai/awesome-deepseek-integration#654).
AUTH_CODES = {40002, 40003}


class AuthRejected(Exception):
    pass


def unwrap(value: str) -> str:
    """Accept the raw token or the localStorage envelope DevTools copies."""
    value = value.strip()
    # DevTools "Copy value" on a localStorage string includes the JSON quotes.
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
        value = value[1:-1].strip()
    # platform.deepseek.com stores userToken as {"value":"...","__version":"0"}
    # rather than a bare string, so a straight copy of the cell lands here.
    if value.startswith("{"):
        try:
            inner = json.loads(value)
        except json.JSONDecodeError:
            return value
        if isinstance(inner, dict):
            v = inner.get("value")
            if isinstance(v, str) and v.strip():
                return v.strip()
    return value


def get_token() -> str | None:
    for var in ("DEEPSEEK_PLATFORM_TOKEN", "DEEPSEEK_USER_TOKEN"):
        v = os.environ.get(var)
        if v and v.strip():
            return unwrap(v)
    try:
        out = subprocess.check_output(
            ["security", "find-generic-password", "-s", kc_service, "-a", kc_account, "-w"],
            stderr=subprocess.DEVNULL,
            text=True,
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None
    return unwrap(out) or None


def fetch(token: str, kind: str, year: int, month: int):
    """GET one month of `amount` or `cost`. Returns the `data` object or None."""
    req = urllib.request.Request(
        f"{api_base}/usage/{kind}?month={month}&year={year}",
        headers={"Authorization": f"Bearer {token}", "Accept": "application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            payload = json.load(resp)
    except urllib.error.HTTPError as e:
        if e.code in (401, 403):
            raise AuthRejected(f"HTTP {e.code}")
        eprint(f"{kind} HTTP {e.code}")
        return None
    except (urllib.error.URLError, TimeoutError) as e:
        eprint(f"{kind} unreachable: {e}")
        return None
    except json.JSONDecodeError:
        eprint(f"{kind} response not JSON")
        return None
    if not isinstance(payload, dict):
        eprint(f"{kind} response shape unrecognized")
        return None
    data = payload.get("data")
    codes = [payload.get("code")]
    if isinstance(data, dict):
        codes.append(data.get("biz_code"))
    # Auth first: an expired userToken is actionable ("re-run
    # --set-deepseek-token"), a generic API error is not. Checking the generic
    # non-zero case first would mask it.
    for c in codes:
        if c in AUTH_CODES:
            raise AuthRejected(f"code {c}")
    if any(c not in (0, None) for c in codes):
        eprint(f"{kind} error code {[c for c in codes if c not in (0, None)]}")
        return None
    return data


def day_totals(biz, decimal_mode: bool) -> dict:
    """Sum non-REQUEST amounts per date, across every model."""
    out: dict[str, Decimal] = {}
    days = biz.get("days") if isinstance(biz, dict) else None
    if not isinstance(days, list):
        return out
    for d in days:
        if not isinstance(d, dict):
            continue
        date = d.get("date")
        if not isinstance(date, str) or len(date) != 10:
            continue
        total = Decimal(0)
        models = d.get("data")
        for m in models if isinstance(models, list) else []:
            if not isinstance(m, dict):
                continue
            items = m.get("usage")
            for u in items if isinstance(items, list) else []:
                # REQUEST is a request count, not tokens or money.
                if not isinstance(u, dict) or u.get("type") == "REQUEST":
                    continue
                try:
                    total += Decimal(str(u.get("amount", "0")))
                except (InvalidOperation, ValueError):
                    continue
        out[date] = total if decimal_mode else int(total)
    return out


def usd_block(data):
    """cost's biz_data is an ARRAY (amount's is an object). Find the USD one."""
    blocks = data.get("biz_data") if isinstance(data, dict) else None
    if not isinstance(blocks, list):
        return None, None
    for b in blocks:
        if isinstance(b, dict) and str(b.get("currency", "")).upper() == "USD":
            return b, "USD"
    cur = next((b.get("currency") for b in blocks if isinstance(b, dict)), None)
    return None, cur


today = dt.datetime.now().astimezone().date()
start = today - dt.timedelta(days=hist_days - 1)
months = [(today.year, today.month)]
if (start.year, start.month) != (today.year, today.month):
    months.insert(0, (start.year, start.month))

token = get_token()
if not token:
    eprint("no platform token in Keychain or DEEPSEEK_PLATFORM_TOKEN")

tok_days: dict = {}
cost_days: dict = {}
amount_ok = False
cost_ok = False
currency = None
auth_ok = True

if token:
    try:
        for year, month in months:
            data = fetch(token, "amount", year, month)
            if data is None:
                continue
            biz = data.get("biz_data") if isinstance(data, dict) else None
            if isinstance(biz, dict):
                tok_days.update(day_totals(biz, decimal_mode=False))
                amount_ok = True
        if amount_ok:
            for year, month in months:
                data = fetch(token, "cost", year, month)
                if data is None:
                    continue
                block, cur = usd_block(data)
                if currency is None:
                    currency = cur
                if block is None:
                    continue
                currency = "USD"
                cost_days.update(day_totals(block, decimal_mode=True))
                cost_ok = True
    except AuthRejected as e:
        eprint(f"auth rejected ({e}) — userToken expired; re-run "
               "codexbar-publish.sh --set-deepseek-token")
        auth_ok = False
        amount_ok = False

if not amount_ok:
    if os.environ.get("DS_CHECK_ONLY"):
        print(json.dumps({"keychain": bool(token), "api": bool(token) and auth_ok,
                          "auth": auth_ok, "ok": False}, separators=(",", ":")))
        sys.exit(3)
    print(json.dumps({"id": "deepseek", "ok": False}, separators=(",", ":")))
    sys.exit(3)

days = [(start + dt.timedelta(days=i)).isoformat() for i in range(hist_days)]
today_iso = today.isoformat()


def fold_future(daily: dict, zero):
    """Fold buckets dated past the local today into the today slot.

    DeepSeek buckets days in its own timezone, which is ahead of US local
    time, so during the local afternoon/evening current usage is filed under
    tomorrow's date. Looking up only start..today would silently drop it for
    up to ~15 hours. Collapsing the overshoot keeps "today" meaning "the
    newest day DeepSeek has", which is the closest honest reading of a series
    we cannot re-bucket.
    """
    folded = dict(daily)
    carry = zero
    for date in [d for d in folded if d > today_iso]:
        carry += folded.pop(date)
    if carry:
        folded[today_iso] = folded.get(today_iso, zero) + carry
    return folded


tok_days = fold_future(tok_days, 0)
cost_days = fold_future(cost_days, Decimal(0))

tok_series = [int(tok_days.get(d, 0)) for d in days]
tt = tok_series[-1]

cost = {"tt": tt, "ht": tok_series}
ct = cw = None
if cost_ok:
    cent_series = [
        int((cost_days.get(d, Decimal(0)) * 100).quantize(Decimal("1"), rounding=ROUND_HALF_UP))
        for d in days
    ]
    ct = cent_series[-1]
    cw = sum(cent_series[-7:])
    cost["ct"] = ct
    cost["cw"] = cw
elif currency:
    eprint(f"cost currency={currency} (not USD) — publishing tokens only")

if os.environ.get("DS_CHECK_ONLY"):
    print(json.dumps({
        "keychain": bool(token),
        "api": True,
        "auth": True,
        "months": len(months),
        "currency": currency,
        "active_days": sum(1 for v in tok_series if v > 0),
        "today_tk": tt,
        "today_cents": ct,
        "ok": True,
    }, separators=(",", ":")))
    sys.exit(0)

print(json.dumps({"id": "deepseek", "ok": True, "cost": cost}, separators=(",", ":")))
eprint(f"today={tt}tok{'' if ct is None else f' ${ct/100:.2f}'} "
       f"hist={len(tok_series)}d max={max(tok_series)}tok months={len(months)}")
PY
