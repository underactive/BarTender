---
date: 2026-05-26T13:00:00-0700
author: Eric Sison
commit: d1328cd
branch: master
repository: bartender
topic: "Cursor token stats on device"
tags: [design, firmware, scripts, cursor, codexbar-publish]
status: ready
parent: .rpiv/artifacts/research/2026-05-26_12-30-00_cursor-token-stats.md
last_updated: 2026-05-26T17:00:00-0700
last_updated_by: Eric Sison
---

# Design: Cursor token stats on device

## Summary

Add `scripts/cursor-stats.sh` to paginate Cursor's dashboard `get-filtered-usage-events` API using a Keychain-stored session cookie, bucket token sums by Mac-local calendar day into `~/.config/codexbar-toy/cursor-history.json`, and emit `{ "id": "cursor", "ok": true, "cu": { tk, mxt, ht } }`. Extend `codexbar-publish.sh` with `CURSOR_MERGE_JXA` (COST_MERGE-style in-place `cu` patch on the existing `cursor` row). Firmware parses `cu` and renders CURSOR TODAY like LM Studio token-only layout (no dollar hero, no requests line).

## Requirements

- Read `WorkosCursorSessionToken` from Keychain (`service=codexbar-toy`, `account=cursor-session`)
- Paginate `POST /api/dashboard/get-filtered-usage-events` with `teamId: 0`, Mac-local 30-day window
- Persist rolling 30-day daily token history on the Mac host only
- Merge `cu` onto existing `cursor` provider without disturbing CodexBar limits (`p`/`s`/`t`)
- Document `cu` in `docs/generated/codexbar-payload.schema.json`
- Firmware CURSOR TODAY: token hero, 30-day chart, 30-day max (LM token branch minus requests)
- Fail-soft: missing credential / API failure → skip `cu`; cached `cu` when history exists and API fails

## Current State Analysis

CodexBar supplies `cursor` limits via `codexbar-stats.sh`; no token totals. Pi/LM precedents exist for host helpers + publisher merge + firmware sub-objects. Schema has `lm` and `pi` but no `cu`. `ui.c` CARD_COST gates on `has_cost || has_lm` only — Cursor limits without cost never show TODAY.

### Key Discoveries

- `scripts/codexbar-publish.sh:169-172` — COST_MERGE_JXA in-place patch template for `cu`
- `scripts/codexbar-publish.sh:388-460` — LM_MERGE_JXA full replace (anti-pattern for Cursor)
- `scripts/codexbar-publish.sh:522-565` — Pi/LM `timeout 30` helper invocation
- `scripts/lmstudio-stats.sh:68,344-347` — history path + cached `ok` semantics
- `scripts/pi-agent-stats.sh:83-89` — `usage_tokens()` field fallback chain
- `firmware/main/stats_model.c:168-238` — `lm` parse branch template for `cu`
- `firmware/main/ui.c:235-236` — CARD_COST gating needs `has_cu`
- `firmware/main/ui.c:1164-1199` — LM TODAY token layout (CURSOR template, minus requests hero)

## Scope

### Building

- `scripts/cursor-stats.sh` (NEW) — API fetch, local history, `cu` emission
- `scripts/codexbar-publish.sh` — `CURSOR_STATS`, `CURSOR_MERGE_JXA`, `cmd_once` stage after LM
- `docs/generated/codexbar-payload.schema.json` — optional `cu` on `id: "cursor"`
- `firmware/main/stats_model.h` / `stats_model.c` — `has_cu`, `cu_*` fields + parse
- `firmware/main/ui.c` — CARD_COST gating + CURSOR TODAY branch

### Not Building

- CodexBar limits sourcing changes
- Cost/$ hero, OpenRouter rows, summary dual-bar (deferred)
- Device-side credentials or history
- `teamId` detection (hardcode `0`)
- Headless browser scraping

## Decisions

### Merge pattern: in-place `cu` patch

**Decision:** `CURSOR_MERGE_JXA` follows `COST_MERGE_JXA` — find `pay.providers[i].id === 'cursor'`, assign `pr.cu`, preserve limits. Not `LM_MERGE_JXA` replace.

**Evidence:** `codexbar-publish.sh:169-172` vs `445-454`; research + discover.

### Keychain: same service, distinct account

**Developer decision (A):** `service=codexbar-toy`, `account=cursor-session` (Upstash write uses `account=publish`).

**Evidence:** `codexbar-publish.sh:53-54`.

### Cached `cu` on API failure

**Decision:** `ok: true` + last-good `cu` from disk when history has token data; mirrors LM Option A.

**Evidence:** `lmstudio-stats.sh:344-347`; discover Q&A.

### `teamId`

**Developer decision (A):** Always send `teamId: 0` in POST body.

### Publisher timeout

**Decision:** `timeout 30` for helper (same as Pi/LM). Pagination must stay within guard.

**Evidence:** `codexbar-publish.sh:549-550`.

### Token field extraction

**Decision:** Prefer `totalTokens` (or `tokenCount`/`tokens`), else sum `input`/`output`/`cache*` numeric fields (Pi `usage_tokens` pattern).

**Evidence:** `pi-agent-stats.sh:83-89`.

### History retention

**Decision:** `cursor-history.json` date → `{ "tk": int }`; retain **30** calendar days; emit `ht` oldest→newest, up to **31** chart points.

**Evidence:** FRD acceptance criteria; `lmstudio-stats.sh:275-278` (31 chart cap).

### Calendar boundary

**Decision:** Mac local timezone for day keys (not Pi UTC).

**Evidence:** discover decision; `pi-agent-stats.sh` uses UTC (explicit contrast).

## Architecture

### `scripts/cursor-stats.sh` — NEW

```zsh
#!/bin/zsh
# cursor-stats.sh — paginate Cursor dashboard usage events and emit a compact
# provider fragment merged by codexbar-publish.sh (in-place cu patch).
#
#   cursor-stats.sh             # emits {"id":"cursor","ok":true,"cu":{...}}
#   cursor-stats.sh --help
#
# Reads:
#   Keychain  service=codexbar-toy  account=cursor-session  (WorkosCursorSessionToken)
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
  *) print -r -- "unknown argument: $1 (try --help)" >&2; exit 2 ;;
esac

PY="${PYTHON3:-$(command -v python3 2>/dev/null || true)}"
[[ -n "$PY" && -x "$PY" ]] || { print -r -- "cursor-stats: python3 not found" >&2; exit 2; }

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
    print(f"cursor-stats: {msg}", file=sys.stderr)


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


def get_session_token() -> str | None:
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
    tok = out.strip()
    return tok or None


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
        "cacheRead",
        "cacheWrite",
        "cacheCreate",
        "cached",
        "cacheHit",
        "cacheMiss",
    ]
    vals = [number(obj.get(k)) for k in parts]
    s = sum(v for v in vals if v is not None)
    return int(round(s)) if any(v is not None for v in vals) else 0


def event_day(ev: dict) -> str | None:
    for key in ("timestamp", "createdAt", "eventTime", "time", "date"):
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


def fetch_events(token: str, start_ms: int, end_ms: int) -> tuple[dict[str, int], bool]:
    daily: dict[str, int] = {}
    page = 1
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
            headers={
                "Content-Type": "application/json",
                "Cookie": f"WorkosCursorSessionToken={token}",
                "User-Agent": "codexbar-toy/1.0",
            },
            method="POST",
        )
        try:
            with urllib.request.urlopen(req, timeout=25) as resp:
                raw = resp.read().decode("utf-8", errors="replace")
        except urllib.error.HTTPError as e:
            eprint(f"API HTTP {e.code} page {page}")
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

        events = None
        if isinstance(payload, list):
            events = payload
        elif isinstance(payload, dict):
            for key in ("events", "usageEvents", "data", "items"):
                if isinstance(payload.get(key), list):
                    events = payload[key]
                    break
        if not events:
            break

        if len(events) == 0:
            break

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

        if len(events) < 100:
            break
        page += 1

    return daily, True


def load_history() -> dict:
    if not hist_file.is_file():
        return {}
    try:
        data = json.loads(hist_file.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        eprint("history file corrupted, starting fresh")
        return {}
    if not isinstance(data, dict):
        return {}
    out = {}
    for k, v in data.items():
        if not isinstance(k, str) or not isinstance(v, dict):
            continue
        tk = v.get("tk", 0)
        try:
            out[k] = {"tk": int(tk)}
        except (TypeError, ValueError):
            continue
    return out


def save_history(history: dict) -> None:
    hist_file.parent.mkdir(parents=True, exist_ok=True)
    tmp = hist_file.with_suffix(".json.tmp")
    try:
        tmp.write_text(json.dumps(history, indent=2), encoding="utf-8")
        tmp.replace(hist_file)
    except OSError as e:
        eprint(f"could not write history: {e}")


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

token = get_session_token()
if token:
    fetched, api_succeeded = fetch_events(token, start_ms, end_ms)
    if api_succeeded:
        for day, tk in fetched.items():
            history[day] = {"tk": int(tk)}
else:
    eprint("no Keychain session token (service=%s account=%s)" % (kc_service, kc_account))

# Prune to 30 calendar days (inclusive window ending today)
sorted_dates = sorted(history.keys(), reverse=True)
keep = set(sorted_dates[:30])
history = {k: v for k, v in history.items() if k in keep}

save_history(history)

sorted_asc = sorted(k for k in history.keys() if k <= today_str)
tok_hist = [history[d].get("tk", 0) for d in sorted_asc]
max_tok = max(tok_hist) if tok_hist else 0
today_tk = history.get(today_str, {}).get("tk", 0)

has_history = any(v.get("tk", 0) > 0 for v in history.values())
today_has_data = today_tk > 0
ok = has_history or (api_succeeded and today_has_data)

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
    },
}
print(json.dumps(provider, separators=(",", ":")))
eprint(
    f"today={today_tk}tok mxt={max_tok}tok hist={len(tok_hist)}d "
    f"api={'ok' if api_succeeded else 'skip'}"
)
PY
```

### `scripts/codexbar-publish.sh:59-61,462-520,567-600` — MODIFY

```zsh
LM_STATS="${CBPUB_LM_STATS:-$SELF_DIR/lmstudio-stats.sh}"
CURSOR_STATS="${CBPUB_CURSOR_STATS:-$SELF_DIR/cursor-stats.sh}"

# Patch `cu` onto the existing CodexBar `cursor` limits row (in-place, like
# COST_MERGE_JXA). Does NOT replace the provider (unlike LM_MERGE_JXA).
read -r -d '' CURSOR_MERGE_JXA <<'EOF'
ObjC.import('Foundation'); ObjC.import('stdlib');
function env(k){ var v=$.NSProcessInfo.processInfo.environment.objectForKey(k);
  return (v && v.js!==undefined) ? String(v.js) : ''; }
function eprint(s){ $.NSFileHandle.fileHandleWithStandardError
  .writeData($.NSString.alloc.initWithUTF8String("cursor-merge: "+s+"\n").dataUsingEncoding(4)); }
function rf(p){ try { var s=$.NSString.stringWithContentsOfFileEncodingError(p,4,null);
  return (s && s.js!==undefined) ? s.js : null; } catch(e){ return null; } }
function i32(v){ var n=Number(v); if(isNaN(n)) return null;
  if(n < -2147483648) n=-2147483648; if(n > 2147483647) n=2147483647;
  return Math.round(n); }
function i64(v){ var n=Number(v); return isNaN(n) ? null : Math.round(n); }
var jsonPath=env('CBPUB_JSON'), curPath=env('CBPUB_CURSOR_JSON');
if(!jsonPath || !curPath){ eprint('missing CBPUB_JSON/CBPUB_CURSOR_JSON'); $.exit(2); }
var pTxt=rf(jsonPath), curTxt=rf(curPath);
if(!pTxt || !curTxt){ eprint('payload/helper unreadable'); $.exit(2); }
var pay, src;
try{ pay=JSON.parse(pTxt); }catch(e){ eprint('payload parse fail'); $.exit(2); }
try{ src=JSON.parse(curTxt); }catch(e){ eprint('helper parse fail'); $.exit(3); }
if(!pay || !Array.isArray(pay.providers)){ eprint('payload shape'); $.exit(2); }
if(!src || src.id!=='cursor' || src.ok!==true || !src.cu || typeof src.cu!=='object' || Array.isArray(src.cu)){
  eprint('helper shape'); $.exit(3);
}
var tk=i32(src.cu.tk), mxt=i64(src.cu.mxt);
if(tk===null || mxt===null){ eprint('helper cu fields'); $.exit(3); }
var ht=[];
if(Array.isArray(src.cu.ht)){
  for(var i=0;i<src.cu.ht.length && ht.length<31;i++){
    var hv=i64(src.cu.ht[i]); if(hv!==null) ht.push(hv);
  }
}
var did=false;
for(var i=0;i<pay.providers.length;i++){
  var pr=pay.providers[i];
  if(pr && pr.id==='cursor'){
    pr.cu={tk:tk, mxt:mxt, ht:ht};
    did=true;
  }
}
if(!did){ eprint('no cursor provider — nothing to merge'); $.exit(0); }
var w=$.NSString.alloc.initWithUTF8String(JSON.stringify(pay))
  .writeToFileAtomicallyEncodingError(jsonPath,true,4,null);
if(!w){ eprint('payload writeback failed'); $.exit(2); }
eprint('merged cursor cu: today='+tk+'tok mxt='+mxt+'tok hist='+ht.length+'d');
$.exit(0);
EOF

  # Patch `cu` onto the existing cursor limits row from cursor-stats.sh.
  # Fail-safe: helper failure -> publish limits-only cursor (never abort).
  local cursor_json="$work/cursor.json"
  local cursor_rc=127
  if [[ -x "$CURSOR_STATS" ]]; then
    if command -v timeout >/dev/null 2>&1; then
      timeout 30 "$CURSOR_STATS" >"$cursor_json" 2>>"$LOG"; cursor_rc=$?
    elif command -v gtimeout >/dev/null 2>&1; then
      gtimeout 30 "$CURSOR_STATS" >"$cursor_json" 2>>"$LOG"; cursor_rc=$?
    else
      "$CURSOR_STATS" >"$cursor_json" 2>>"$LOG"; cursor_rc=$?
    fi
    if [[ $cursor_rc -eq 0 ]]; then
      if CBPUB_JSON="$json" CBPUB_CURSOR_JSON="$cursor_json" osascript -l JavaScript -e "$CURSOR_MERGE_JXA" 2>>"$LOG"; then
        bytes=$(wc -c <"$json" | tr -d ' ')
      else
        log "note: Cursor token merge skipped (malformed helper output) — publishing limits-only cursor"
      fi
    elif [[ $cursor_rc -eq 124 ]]; then
      log "note: Cursor stats helper timed out after 30s — publishing limits-only cursor"
    else
      log "note: Cursor stats helper failed (exit code $cursor_rc) — publishing limits-only cursor"
    fi
  else
    log "note: Cursor stats skipped (absent/unrecognized) — publishing limits-only cursor"
  fi
```

### `docs/generated/codexbar-payload.schema.json` — MODIFY

```json
"cu": {
  "type": "object",
  "description": "v2 optional, Cursor provider only. Mac-local daily token rollup from scripts/cursor-stats.sh merged by codexbar-publish.sh. Present only on provider id `cursor`; WorkosCursorSessionToken never published.",
  "required": ["tk", "mxt", "ht"],
  "properties": {
    "tk":  { "type": "integer", "description": "Tokens today (Mac-local calendar day)." },
    "mxt": { "type": "integer", "description": "30-day max daily tokens." },
    "ht":  {
      "type": "array",
      "items": { "type": "integer" },
      "maxItems": 31,
      "description": "Daily token totals, oldest -> newest. Firmware caps at STATS_HIST_MAX."
    }
  },
  "additionalProperties": false
}
```

Top-level `description` (line 5) gains: `scripts/cursor-stats.sh` + optional per-provider `cu` on `cursor`.

### `firmware/main/stats_model.h:87-90` — MODIFY

```c
    float    lm_week_ch[LM_WEEK_MAX];

    // v2 optional `cu` block: Cursor token rollup (Mac-local daily totals).
    // Token-only — no cost/requests. Dedicated fields (not cost-slot reuse).
    bool     has_cu;
    int64_t  cu_tok_today;              // tokens today (Mac-local calendar day)
    int64_t  cu_tok_month_max;          // 30-day max daily tokens
    int      cu_ht_n;                   // valid entries in cu_ht[]
    int64_t  cu_ht[STATS_HIST_MAX];     // daily tokens, oldest -> newest
} stats_provider_t;
```

### `firmware/main/stats_model.c:238-240` — MODIFY

```c
                if (any_lm) p->has_lm = true;
            }
            // v2 optional `cu` block: Cursor publishes Mac-local token rollup
            // from cursor-stats.sh (no cost/requests).
            const cJSON *cu = cJSON_GetObjectItemCaseSensitive(e, "cu");
            if (strcmp(p->id, "cursor") == 0 && cJSON_IsObject(cu)) {
                const cJSON *x;
                bool any_cu = false;
                x = cJSON_GetObjectItemCaseSensitive(cu, "tk");
                if (cJSON_IsNumber(x)) { p->cu_tok_today = i64_clamp(x->valuedouble); any_cu = true; }
                x = cJSON_GetObjectItemCaseSensitive(cu, "mxt");
                if (cJSON_IsNumber(x)) { p->cu_tok_month_max = i64_clamp(x->valuedouble); any_cu = true; }
                const cJSON *ht = cJSON_GetObjectItemCaseSensitive(cu, "ht");
                if (cJSON_IsArray(ht)) {
                    const cJSON *hv;
                    cJSON_ArrayForEach(hv, ht) {
                        if (p->cu_ht_n >= STATS_HIST_MAX) break;
                        if (cJSON_IsNumber(hv)) {
                            p->cu_ht[p->cu_ht_n++] = i64_clamp(hv->valuedouble);
                            any_cu = true;
                        }
                    }
                }
                if (any_cu) p->has_cu = true;
            }
            // v2 optional `ph`: 24h usage-% history (provider-level,
```

### `firmware/main/ui.c:172-189,217,235-236,252,1146-1200,1757-1782` — MODIFY

```c
    h = hash_mix_u32(h, p->has_lm ? 1U : 0U);
    if (p->has_lm) {
        /* ... existing lm_* hash fields ... */
    }
    h = hash_mix_u32(h, p->has_cu ? 1U : 0U);
    if (p->has_cu) {
        h = hash_mix_u32(h, (uint32_t)p->cu_tok_today);
        h = hash_mix_u32(h, (uint32_t)(p->cu_tok_today >> 32));
        h = hash_mix_u32(h, (uint32_t)p->cu_tok_month_max);
        h = hash_mix_u32(h, (uint32_t)(p->cu_tok_month_max >> 32));
        h = hash_mix_u32(h, (uint32_t)p->cu_ht_n);
        for (int i = 0; i < p->cu_ht_n && i < STATS_HIST_MAX; i++)
            h = hash_mix_u32(h, (uint32_t)p->cu_ht[i]);
    }
    return h;
}

        if (!p->has_cost && !provider_has_limits_card(p) && !p->has_lm && !p->has_cu) continue;

        case CARD_COST:         return p->has_cost || p->has_lm || p->has_cu;

        if (p->has_cost || p->has_lm || p->has_cu) *card = CARD_COST;

    bool is_lmstudio = (strcmp(p->id, "lmstudio") == 0);
    bool is_cursor = (strcmp(p->id, "cursor") == 0);

        if (!p->has_cost && !p->has_lm && !p->has_cu) {
            /* cost_na branch */
        }

        if (is_lmstudio) {
            /* existing LM TODAY branch — unchanged */
            return;
        }

        if (is_cursor && p->has_cu) {
            // CURSOR TODAY: token hero only (no requests line, no $, no OR rows)
            lv_obj_t *hide[] = { cost_or_lbl, cost_or_row1, cost_or_row2,
                                 cost_bar, cost_bar_lbl, cost_cap,
                                 cost_tok, cost_tok_unit };
            for (unsigned i = 0; i < sizeof hide / sizeof *hide; i++)
                lv_obj_add_flag(hide[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(cost_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(cost_lbl, "TOKENS");
            lv_obj_t *show[] = { cost_big, cost_30, cost_chart };
            for (unsigned i = 0; i < sizeof show / sizeof *show; i++)
                lv_obj_clear_flag(show[i], LV_OBJ_FLAG_HIDDEN);
            const int scr_w = lv_display_get_horizontal_resolution(lv_display_get_default());
            const int scr_h = lv_display_get_vertical_resolution(lv_display_get_default());
            lv_obj_set_pos(cost_30, 12, scr_h - 22);
            lv_obj_set_size(cost_chart, scr_w - 24, scr_h - 162);
            char tk[16], tk30[16];
            fmt_tokens(tk, sizeof tk, p->cu_tok_today);
            lv_label_set_text(cost_big, tk);
            fmt_tokens(tk30, sizeof tk30, p->cu_tok_month_max);
            lv_label_set_text_fmt(cost_30, "30 DAY MAX: %s Toks", tk30);
            int n = p->cu_ht_n;
            if (n > NAV_HIST_PTS) n = NAV_HIST_PTS;
            lv_color_t cc;
            int32_t ht32[STATS_HIST_MAX];
            for (int hi = 0; hi < n && hi < STATS_HIST_MAX; hi++)
                ht32[hi] = (int32_t)(p->cu_ht[hi] > INT32_MAX ? INT32_MAX : p->cu_ht[hi]);
            (void)render_cost_bar_chart(cost_chart, cost_ser, ht32, n,
                prov_accent(p->id, &cc) ? cc : lv_color_hex(0x7C3AED));
            if (card_entered) anim_chart_fadein(cost_chart);
            return;
        }

                if (strcmp(tp->id, "lmstudio") == 0
                    || (strcmp(tp->id, "cursor") == 0 && tp->has_cu)) {
                    st.nav_card = CARD_COST;
                } else {

            const stats_provider_t *np = &st.stats.p[st.nav_provider];
            if (strcmp(np->id, "lmstudio") == 0) {
                st.nav_card = (st.nav_card == CARD_COST) ? CARD_LIMITS
                    : (np->has_cost ? CARD_COST : CARD_LIMITS);
            } else if (strcmp(np->id, "cursor") == 0 && np->has_cu) {
                st.nav_card = (st.nav_card == CARD_COST) ? CARD_LIMITS : CARD_COST;
            } else {
                st.nav_card = (st.nav_card == CARD_COST) ? CARD_LIMITS
                    : (np->has_cost ? CARD_COST : CARD_LIMITS);
            }
```

## Slices

### Slice 1: Host stats helper

**Files**: `scripts/cursor-stats.sh`

#### Automated Verification:
- [ ] Shell syntax check: `zsh -n scripts/cursor-stats.sh`
- [ ] Help exits 0: `scripts/cursor-stats.sh --help`
- [ ] Unknown arg exits 2: `scripts/cursor-stats.sh --bogus`; echo exit=$?

#### Manual Verification:
- [ ] Keychain item present: `security find-generic-password -s codexbar-toy -a cursor-session` (no `-w` in logs)
- [ ] Successful run stdout parses as JSON with `id=="cursor"`, `ok==true`, `cu.tk`, `cu.mxt`, `cu.ht` array
- [ ] History written: `~/.config/codexbar-toy/cursor-history.json` has date keys → `{ "tk": int }`
- [ ] No token leakage: `scripts/cursor-stats.sh 2>&1 | rg -i 'WorkosCursorSessionToken|eyJ'` returns empty
- [ ] API failure + existing history: disconnect network, re-run → `ok: true` with last-good `cu` (stale `tk` acceptable)
- [ ] No credential + empty history: remove Keychain item, delete history → exit 3, `ok: false`

### Slice 2: Publisher merge + pipeline

**Files**: `scripts/codexbar-publish.sh`

#### Automated Verification:
- [ ] Shell syntax check: `zsh -n scripts/codexbar-publish.sh`
- [ ] Merge JXA present: `rg -n 'CURSOR_MERGE_JXA|CBPUB_CURSOR_JSON' scripts/codexbar-publish.sh | wc -l` returns >= 4
- [ ] Helper path wired: `rg -n 'CURSOR_STATS' scripts/codexbar-publish.sh`

#### Manual Verification:
- [ ] `./scripts/codexbar-publish.sh --once` with valid Keychain + cursor in CodexBar payload: published JSON has `cursor` with both limits (`p`/`s`/`t`) and `cu.{tk,mxt,ht}`
- [ ] Helper missing or exit 3: publish succeeds; `cursor` row has limits only (no `cu`)
- [ ] Helper timeout (simulate slow helper): log contains `timed out after 30s`; publish continues
- [ ] Malformed `cursor.json`: log `Cursor token merge skipped`; publish continues

### Slice 3: Payload contract

**Files**: `docs/generated/codexbar-payload.schema.json`

#### Automated Verification:
- [ ] Schema parses: `python3 -c "import json; json.load(open('docs/generated/codexbar-payload.schema.json'))"`
- [ ] `cu` block present: `rg -n '"cu"' docs/generated/codexbar-payload.schema.json | wc -l` returns >= 1

#### Manual Verification:
- [ ] `cu` sits inside provider `properties` after `lm`, before closing `}` of provider items
- [ ] `cu` required keys `tk`, `mxt`, `ht` match `cursor-stats.sh` stdout and `CURSOR_MERGE_JXA` coercion
- [ ] Provider `required` unchanged (`id`, `ok` only) — `cu` remains optional

### Slice 4: Firmware parse

**Files**: `firmware/main/stats_model.h`, `firmware/main/stats_model.c`

#### Automated Verification:
- [ ] Firmware builds: `cd firmware && idf.py build`
- [ ] Parse symbols present: `rg -n 'has_cu|cu_tok_today|cu_ht' firmware/main/stats_model.c firmware/main/stats_model.h`

#### Manual Verification:
- [ ] Fixture with `{"id":"cursor","ok":true,"cu":{"tk":100,"mxt":500,"ht":[10,20,100]}}` sets `has_cu`, `cu_tok_today==100`, `cu_tok_month_max==500`, `cu_ht_n==3`
- [ ] `cu` on non-cursor provider ignored (`has_cu` stays false)
- [ ] Partial `cu` (e.g. only `ht`) still sets `has_cu` when any field parses

### Slice 5: Firmware CURSOR TODAY UI

**Files**: `firmware/main/ui.c`

#### Automated Verification:
- [ ] Firmware builds: `cd firmware && idf.py build`
- [ ] Gating wired: `rg -n 'has_cu' firmware/main/ui.c | wc -l` returns >= 6
- [ ] Cursor branch present: `rg -n 'is_cursor|cu_tok_today|cu_ht' firmware/main/ui.c`

#### Manual Verification:
- [ ] Cursor with `cu` only (no `cost`): summary row visible; tap opens CARD_COST (TODAY)
- [ ] CURSOR TODAY shows token hero, `30 DAY MAX: … Toks` (no requests bullet, no $)
- [ ] OR rows, cost bars, `cost_tok` requests line hidden on CURSOR TODAY
- [ ] Tap toggles TODAY ↔ Usage Limits when CodexBar limits present
- [ ] Nav hash / screensaver activity updates when `cu.tk` changes (provider_metric_sig includes `cu_*`)

## Desired End State

```bash
# Store session cookie once (manual; not scripted here)
security add-generic-password -U -s codexbar-toy -a cursor-session -w

# Host helper emits cu fragment
./scripts/cursor-stats.sh
# {"id":"cursor","ok":true,"cu":{"tk":12345,"mxt":89012,"ht":[...]}}

# Publish merges cu onto existing cursor limits row
./scripts/codexbar-publish.sh --once

# Device shows CURSOR TODAY with token hero + 30-day chart when cu present
```

## File Map

- `scripts/cursor-stats.sh` — NEW — Cursor API fetch + history + `cu` emission
- `scripts/codexbar-publish.sh` — MODIFY — `CURSOR_MERGE_JXA` + `cmd_once` stage
- `docs/generated/codexbar-payload.schema.json` — MODIFY — `cu` sub-object
- `firmware/main/stats_model.h` — MODIFY — `has_cu`, `cu_*` fields
- `firmware/main/stats_model.c` — MODIFY — parse `cu` on `cursor`
- `firmware/main/ui.c` — MODIFY — CARD_COST + CURSOR TODAY branch

## Ordering Constraints

1. Slice 1 (helper + field names) before publisher merge and schema
2. Schema before firmware parse (contract)
3. Parse before UI (needs `has_cu` / `cu_ht_n`)
4. Slices sequential; no parallel implementation

## Verification Notes

- `zsh -n scripts/cursor-stats.sh` — syntax check
- `./scripts/cursor-stats.sh` with valid Keychain token — stdout has `cu.tk`, `cu.mxt`, `cu.ht`
- `./scripts/codexbar-publish.sh --once` — merged payload preserves `cursor` limits + adds `cu`
- `cd firmware && idf.py build` — firmware compiles after slices 4–5
- Grep payload/logs for session token strings — must be absent
- Compare `cu.tk` / daily sums to cursor.com/dashboard/usage (Mac local days)

## Performance Considerations

- Full 30-day pagination may be heavy; `pageSize` 100, stop early on empty page
- Publisher `timeout 30` caps helper runtime; log skip on exit 124
- History write via temp file + rename to avoid partial JSON

## Migration Notes

Not applicable — new feature; no persisted device schema change beyond v2 superset `cu`.

## Pattern References

- `scripts/lmstudio-stats.sh` — zsh + inline Python, rolling history, `ok` semantics
- `scripts/pi-agent-stats.sh:83-89` — token field fallback
- `scripts/codexbar-publish.sh:169-172` — in-place provider patch
- `firmware/main/stats_model.c:168-238` — `lm` parse
- `firmware/main/ui.c:1164-1199` — LM TODAY token UI

## Developer Context

**Q: Keychain location for WorkosCursorSessionToken?**  
A: Option A — `service=codexbar-toy`, `account=cursor-session` (`codexbar-publish.sh:53-54` precedent).

**Q: `teamId` for get-filtered-usage-events?**  
A: Option A — hardcode `0`.

**Q: Cached `cu` when API fails but history exists?**  
A: Option A (discover) — `ok: true` + last-good `cu` (`lmstudio-stats.sh:344-347`).

**Q: Proceed to decomposition?**  
A: Proceed.

**Q: Approve decomposition (5 slices)?**  
A: Approve.

**Slice 1 micro-checkpoint:** Approve as generated.

**Slice 2 micro-checkpoint:** Approve as generated.

**Slice 3 micro-checkpoint:** Approve as generated.

**Slice 4 micro-checkpoint:** Approve as generated.

**Slice 5 micro-checkpoint:** Approve as generated.

## Design History

- Slice 1: Host stats helper — approved as generated
- Slice 2: Publisher merge + pipeline — approved as generated
- Slice 3: Payload contract — approved as generated
- Slice 4: Firmware parse — approved as generated
- Slice 5: Firmware CURSOR TODAY UI — approved as generated

## References

- `.rpiv/artifacts/research/2026-05-26_12-30-00_cursor-token-stats.md`
- `.rpiv/artifacts/discover/2026-05-26_12-00-00_cursor-token-stats.md`
- `.rpiv/artifacts/designs/2026-05-23_12-04-10_lm-studio-stats-on-device.md`
- `docs/SECURITY.md`
