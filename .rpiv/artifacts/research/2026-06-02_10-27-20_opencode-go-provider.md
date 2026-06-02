---
date: 2026-06-02T10:27:20-0700
author: Eric Sison
commit: d4f6f44
branch: main
repository: bartender
topic: "Add OpenCode Go provider to firmware pipeline (unhide + host script + publisher merge + UI cards)"
tags: [research, firmware, scripts, opencodego, provider]
status: complete
last_updated: 2026-06-02T10:27:20-0700
last_updated_by: Eric Sison
---

# Research: OpenCode Go Provider for BarTender

## Research Question
Add the OpenCode Go provider to the BarTender pipeline: unhide it from HIDDEN_PROVIDERS, add a dedicated `oc` sub-object for token/cost data scraped from the OpenCode website API, add a host-side scraper script (`opencodego-stats.sh`) and publisher merge stage, render a custom TODAY page (tokens hero + cost line + 30-day token chart) and a LIMITS page following the Cursor three-tier template (p/s/t usage bars), placed between LM Studio and OpenRouter in the provider display order.

## Summary
The OpenCode Go provider already has icons (`ic_opencode`), accent color (`0x3B82F6` blue), and a HIDDEN_PROVIDERS entry — but zero pipeline plumbing. Adding it requires changes across 15+ files in 5 layers:

1. **Host script**: New `scripts/opencodego-stats.sh` scrapes OpenCode website API (token counts + cost + 30-day history) using a Keychain-stored auth cookie, following the Cursor `cursor-stats.sh` pattern.
2. **Publisher merge**: New `OG_MERGE_JXA` block in `codexbar-publish.sh` patches the `oc` sub-object onto the existing CodexBar `opencodego` provider entry (which already carries p/s/t usage bars from `codexbar-stats.sh --json`). In-place patch like CURSOR_MERGE_JXA, positioned after LM Studio merge.
3. **Firmware data model**: New dedicated `oc` block on `stats_provider_t` with `has_oc`, `oc_tok_today` (int64), `oc_cost_today_c` (int32), `oc_tok_month_max` (int64), and `oc_ht[]` (int64 history). Parsed by `parse_oc()` gated on strcmp(p->id, "opencodego").
4. **Firmware UI**: PK_OPENCODEGO enum, unhide from HIDDEN_PROVIDERS, add to s_display_order, custom TODAY render path (tokens hero + cost line + bar chart), LIMITS page via Cursor three-tier template (p/s/t).
5. **JSON schema**: Declare `oc` sub-object with additionalProperties: false.

## Detailed Findings

### Provider Registration & Unhiding

#### Current hidden state (`ui_internal.h:59`)
```c
#define HIDDEN_PROVIDERS "ollama", "opencode", "opencodego"
```
Un-hiding means removing `"opencodego"` from this macro. The `is_hidden_provider()` function at `ui_format.c:130` consumes this macro into a flat string list — unhiding works by macro edit alone, no enum change needed.

#### Already registered
- `provider_colors.h:18`: `{ "opencodego", 0x3B82F6 }` (blue accent)
- `provider_icons.c:6385,6426,6467`: all three lookup tables map `"opencodego"` → `ic_opencode` (A8 silhouette)
- `scripts/build/gen-provider-icons.py:61`: `"opencodego": "opencode"` — shares the OpenCode SVG
- Can optionally add a distinct `opencodego.svg` asset later

#### Display order insertion (`stats_model.c:343-351`)
Current `s_display_order[]`:
```c
"pi", "lmstudio", "openrouter", "claude", "codex", "cursor"
```
Insert `"opencodego"` at position 2 (after `lmstudio`, before `openrouter`):
```c
"pi", "lmstudio", "opencodego", "openrouter", "claude", "codex", "cursor"
```
This changes the priority range from 0–5 to 0–6. The bubble sort at lines 365-376 handles this correctly — unknown providers remain prio -1 and sink to the end. The `test_reorder_unknown_sinks_to_end()` test is unaffected.

#### Provider kind enum (`ui_internal.h:81-87`)
Add `PK_OPENCODEGO` to the enum. The `provider_kind()` resolver at `ui_format.c:145` gets a new `strcmp(id, "opencodego") == 0` branch. `summary_provider_name()` at `ui_format.c:165` gets a `case PK_OPENCODEGO: return "OpenCode Go";`.

All `switch(pk)` dispatch points in `ui_render.c` — the data-driven paths (`has_cost`, `has_balance`, `default` branches) handle OpenCode Go without requiring explicit `PK_OPENCODEGO` cases at most of them. The exceptions:
- `render_cost_card()` at `ui_render.c:1143` — needs a PK_OPENCODEGO branch for custom TODAY rendering
- `provider_tok_today()` at `ui_format.c:215` — needs a PK_OPENCODEGO branch returning `p->oc_tok_today`
- `provider_has_both_cards()` at `ui.c:244` — needs `PK_OPENCODEGO && p->has_oc`

### Data Model — Dedicated `oc` Block

#### Why dedicated block (not generic `cost` block)
The user confirmed CodexBar provides p/s/t usage bars for OpenCode Go via `codexbar-stats.sh --json`, but NOT token/cost data. A separate OpenCode website API scraper provides:
- Today's tokens (int64) — for hero widget
- Today's cost in cents (int32) — for secondary tok line  
- 30-day token history (int64 array) — for chart + footer max
- The chart shows TOKENS (single history series, not cost)

The dedicated `oc` block pattern follows the Cursor `cu` precedent, keeping the data model clean (token-focused, no fake spend fields). Fields to add to `stats_provider_t`:

```c
// v2 optional `oc` block: OpenCode Go token rollup + cost from website API
bool     has_oc;
int64_t  oc_tok_today;              // tokens today (from OpenCode API)
int32_t  oc_cost_today_c;           // cost today in cents (from OpenCode API)
int64_t  oc_tok_month_max;          // 30-day max daily tokens
int      oc_ht_n;                   // valid entries in oc_ht[]
int64_t  oc_ht[STATS_HIST_MAX];     // daily token totals, oldest -> newest
```

#### Parse function (`stats_model.c`)
Following the `parse_cu()` pattern at lines 217-241:

```c
static void parse_oc(const cJSON *e, stats_provider_t *p)
{
    const cJSON *oc = cJSON_GetObjectItemCaseSensitive(e, "oc");
    if (strcmp(p->id, "opencodego") != 0 || !cJSON_IsObject(oc)) return;
    bool any_oc = false;
    if (get_i64(oc, "tk",  &p->oc_tok_today))    any_oc = true;
    if (get_i32(oc, "ct",  &p->oc_cost_today_c)) any_oc = true;
    if (get_i64(oc, "mxt", &p->oc_tok_month_max)) any_oc = true;
    const cJSON *ht = cJSON_GetObjectItemCaseSensitive(oc, "ht");
    if (cJSON_IsArray(ht)) {
        const cJSON *hv;
        cJSON_ArrayForEach(hv, ht) {
            if (p->oc_ht_n >= STATS_HIST_MAX) break;
            if (cJSON_IsNumber(hv)) {
                p->oc_ht[p->oc_ht_n++] = i64_clamp(hv->valuedouble);
                any_oc = true;
            }
        }
    }
    if (any_oc) p->has_oc = true;
}
```

Called from the parse loop alongside `parse_cost()`, `parse_pi()`, `parse_lm()`, `parse_cu()`, `parse_ph()`.

#### JSON schema addition (`docs/generated/codexbar-payload.schema.json`)
Add `oc` as an optional property on each provider entry (alongside `cost`, `pi`, `lm`, `cu`):

```json
"oc": {
    "type": "object",
    "description": "v2 optional, OpenCode Go provider only. Token rollup + today's cost from opencode.ai API. Present only on provider id `opencodego`.",
    "required": ["tk", "ct", "mxt", "ht"],
    "properties": {
        "tk":  { "type": "integer", "description": "Tokens today (local calendar day)." },
        "ct":  { "type": "integer", "description": "Cost today in cents." },
        "mxt": { "type": "integer", "description": "30-day max daily tokens." },
        "ht":  {
            "type": "array",
            "items": { "type": "integer" },
            "maxItems": 31,
            "description": "Daily token totals, oldest -> newest."
        }
    },
    "additionalProperties": false
}
```

#### Provider card availability branching
Update `provider_card_available()` at `ui_format.c:126-133`:
```c
case CARD_COST: return p->has_cost || p->has_lm || p->has_cu || p->has_oc;
```
This makes TODAY card available when the `oc` block is present.

Update `provider_has_both_cards()` at `ui.c:244-248`:
```c
return pk == PK_LMSTUDIO || (pk == PK_CURSOR && p->has_cu) || (pk == PK_OPENCODEGO && p->has_oc);
```
This enables the 2-card tap cycle (TODAY ↔ LIMITS) when the `oc` block is present.

Update `provider_tok_today()` at `ui_format.c:215-221`:
```c
case PK_OPENCODEGO: return p->has_oc ? p->oc_tok_today : 0;
```

### UI — TODAY Card (CARD_COST)

The TODAY page follows the user's grid layout:
- **Rows 0-1, 2 cols**: Hero widget showing "TOKENS" (caption) + token count (number)
- **Row 2, 2 cols**: Cost line "$1.23" (cost.tok)
- **Rows 3-8, 2 cols**: 30-day token bar chart
- **Footer**: "max tokens . max spend" showing `oc_tok_month_max` + `oc_cost_today_c` (today's cost)

This requires a custom render path in `render_cost_card()` at `ui_render.c:1143`. Add:
```c
case PK_OPENCODEGO:
    if (p->has_oc) {
        render_opencodego_today(p, g, hero, body, footer, card_entered);
        return;
    }
    break;
```

The `render_opencodego_today()` function:
1. Sets chrome: up_id("OPENCODEGO") + subtitle "TODAY"
2. Places hero_amount at rows 0-1 with caption "TOKENS" and count via `oc_tok_today`
3. Places cost.tok at row 2 with fmt_money from `oc_cost_today_c` (displays "$1.23")
4. Renders bar chart at rows 3-8 using `oc_ht[]` (daily token history)
5. Footer shows oc_tok_month_max as "max tokens" and oc_cost_today_c as "max spend"

The template to model after is `render_token_chart_card()` at `ui_render.c:908-993` (used by Cursor/LM Studio TODAY pages), which places a tokens hero + chart. The cost line (row 2) is the new addition.

### UI — LIMITS Card (CARD_LIMITS)

The LIMITS page follows the Cursor three-tier layout (`render_limits_card()` at `ui_render.c:1369-1446`):
- **Hero**: TOTAL caption (since tertiary has data), primary bar → 5-hour usage
- **AUTO tier** (grid row 4): secondary bar → weekly usage
- **API tier** (footer): tertiary bar → monthly usage
- **Sparkline**: hidden (no `ph` array)

The usage bars map 1:1 to CodexBar output fields:
| Bar | CodexBar field | LIMITS tier |
|-----|---------------|-------------|
| 5-hour usage | `p` (primary) | Hero SESSION → total |
| Weekly usage | `s` (secondary) | AUTO tier |
| Monthly usage | `t` (tertiary) | API tier |

The Cursor template renders these via `render_limits_auto()` (secondary tier, label "AUTO" when tertiary present) and `render_limits_weekly()` (tertiary tier, label "API"). No custom render function needed — the data-driven paths handle it automatically when `primary.has`, `secondary.has`, `tertiary.has` are all true.

### Host Script — `scripts/opencodego-stats.sh`

Follows the `cursor-stats.sh` pattern:
- **Language**: zsh wrapper with inline JXA or Python heredoc
- **Auth**: Reads auth cookie from Keychain via `security find-generic-password` (same `KC_SERVICE=codexbar-toy`, dedicated `KC_ACCOUNT` like `cursor-session`)
- **API call**: `curl` to `https://opencode.ai/workspace/wrk_01KT1W5K2X3FPZCZVFYYJWBEW8/usage` with Cookie header from Keychain
- **Output shape**: `{"id":"opencodego","ok":true,"og":{"tk":...,"ct":...,"mxt":...,"ht":[...]}}`
- **History**: Rolling history file at `~/.config/codexbar-toy/opencodego-history.json` for 30-day persistence (using `scripts/lib/_stats_history.py`)
- **Timeouts**: 30-second timeout guard in `cmd_once()`
- **Env overrides**: `OPENCODE_GO_COOKIE`, `OPENCODE_GO_HISTORY`, `OPENCODE_GO_API_URL` for hermetic testing

The auth cookie setup mirrors the Cursor flow:
- `codexbar-publish.sh --set-opencodego-cookie` — prompts user to paste Cookie header, stores in Keychain
- `codexbar-publish.sh --set-opencodego-cookie-clipboard` — reads from clipboard via `pbpaste`
- Stored as `KC_SERVICE=codexbar-toy`, `KC_ACCOUNT=opencodego-session`

### Publisher Merge — `codexbar-publish.sh`

#### Merge stage placement
Insert after LM Studio merge (line ~708), before Cursor cu merge (line ~711):
```
Step 5: LM Studio merge   (line 674-708)
Step 5b: OpenCode Go merge  ← NEW (after line 708)
Step 6: Cursor cu merge   (line 711+)
```

#### Merge pattern: In-place patch (like CURSOR_MERGE_JXA)
OpenCode Go already exists in the CodexBar base payload (with p/s/t bars). The merge patches the `oc` block onto it — no replacement or reordering needed.

```zsh
read -r -d '' OPENCODE_GO_MERGE_JXA <<'EOF'
// Reads CBPUB_JSON and CBPUB_OG_JSON
// Validates src.id==='opencodego', src.ok===true, src.og as object
// Extracts og.tk, og.ct, og.mxt (required)
// Optionally forwards og.ht
// Finds opencodego provider in pay.providers[], patches oc block in-place
// On success: exit 0 (payload mutated); on failure: exit non-zero (skipped)
EOF
```

#### Script variable and config
```zsh
OPENCODE_GO_STATS="${CBPUB_OPENCODE_GO_STATS:-$SELF_DIR/opencodego-stats.sh}"
KC_ACCOUNT_OG="${CBPUB_KC_ACCOUNT_OG:-opencodego-session}"
```

#### Guard block (following LM Studio pattern at lines 674-708)
```zsh
local og_json="$work/opencodego.json"
local og_rc=127
if [[ -x "$OPENCODE_GO_STATS" ]]; then
  if command -v timeout >/dev/null 2>&1; then
    timeout 30 "$OPENCODE_GO_STATS" >"$og_json" 2>>"$LOG"; og_rc=$?
  ...
  if [[ $og_rc -eq 0 ]]; then
    if CBPUB_JSON="$json" CBPUB_OG_JSON="$og_json" osascript -l JavaScript -e "$OPENCODE_GO_MERGE_JXA" 2>>"$LOG"; then
      bytes=$(wc -c <"$json" | tr -d ' ')
    else
      log "note: OpenCode Go merge skipped — publishing without OpenCode Go"
    fi
  elif [[ $og_rc -eq 124 ]]; then
    log "note: OpenCode Go helper timed out after 30s — publishing without OpenCode Go"
  else
    log "note: OpenCode Go helper failed (exit code $og_rc) — publishing without OpenCode Go"
  fi
fi
```

### Firmware Test Coverage

#### Reorder test (`test_stats_model.c`)
New test `test_reorder_opencodego_insertion()` verifying insertion between lmstudio and openrouter:
- Input: `openrouter, lmstudio, opencodego, pi, cursor`
- Expected after sort: `pi, lmstudio, opencodego, openrouter, cursor`

#### Parse test
New `test_opencodego_provider_parsed()` following `test_cursor_provider_parsed()` pattern:
- JSON payload with `oc` block
- Verify `has_oc=true`, `oc_tok_today`, `oc_cost_today_c`, `oc_tok_month_max`, `oc_ht[]`

#### Existing tests unaffected
- `test_reorder_unknown_sinks_to_end()` — passes unchanged
- All `test_*_cap` limit tests — unaffected by added array

## Code References

### Core
- `firmware/main/stats_model.h:13-46` — stats_provider_t struct (add oc fields)
- `firmware/main/stats_model.c:343-351` — s_display_order[] (insert opencodego)
- `firmware/main/stats_model.c:217-241` — parse_cu() pattern for parse_oc()
- `firmware/main/ui_internal.h:59` — HIDDEN_PROVIDERS macro (remove opencodego)
- `firmware/main/ui_internal.h:81-87` — provider_kind_t enum (add PK_OPENCODEGO)
- `firmware/main/ui_format.c:126-133` — provider_card_available() (add has_oc)
- `firmware/main/ui_format.c:145-163` — provider_kind() resolver (add opencodego branch)
- `firmware/main/ui_format.c:165-176` — summary_provider_name() (add "OpenCode Go")
- `firmware/main/ui_format.c:215-221` — provider_tok_today() (add PK_OPENCODEGO case)
- `firmware/main/ui.c:244-248` — provider_has_both_cards() (add PK_OPENCODEGO)
- `firmware/main/ui.c:251-255` — initial_card_for() (unchanged — data-driven)
- `firmware/main/ui.c:258-264` — next_card_for() (unchanged — data-driven)
- `firmware/main/ui_render.c:1143-1160` — render_cost_card() switch (add PK_OPENCODEGO branch)
- `firmware/main/ui_render.c:908-993` — render_token_chart_card() (model for opencodego today)
- `firmware/main/ui_render.c:1493-1517` — render_summary_secondary_bar() (default=hidden, ok)
- `firmware/main/provider_colors.h:18` — { "opencodego", 0x3B82F6 } (already registered)
- `firmware/main/provider_icons.c:6385,6426,6467` — opencodego icon tables (already registered)

### Host
- `scripts/codexbar-publish.sh:55-65` — env var declarations (add OPENCODE_GO_STATS)
- `scripts/codexbar-publish.sh:458-527` — LM_MERGE_JXA template for OG_MERGE_JXA
- `scripts/codexbar-publish.sh:531-577` — CURSOR_MERGE_JXA (in-place patch pattern)
- `scripts/codexbar-publish.sh:674-708` — LM Studio merge block (template for OG merge)
- `scripts/codexbar-publish.sh:116-131` — store_cursor_session() pattern for OpenCode Go
- `scripts/cursor-stats.sh:1-527` — full scraper pattern (API fetch, history, env overrides)

### Tests
- `firmware/test/stats_model/test_stats_model.c:676-706` — reorder test (insert new test)
- `firmware/test/stats_model/test_stats_model.c:850-870` — cursor parse test (model parse test)
- `firmware/test/stats_model/test_stats_model.c:708-725` — unknown sinks test (unaffected)

### Schema
- `docs/generated/codexbar-payload.schema.json:85-98` — pi block template for oc block

## Integration Points

### Inbound References
- `scripts/codexbar-publish.sh:600-606` — base payload generation includes opencodego via codexbar-stats.sh
- `scripts/codexbar-publish.sh:674-708` — LM merge stage (relative insertion point)
- `firmware/main/fetch.c:46-52` — do_fetch() → stats_model_parse() → ui_set_stats()
- `firmware/main/ui.c:286` — initial_card_for() on summary tap
- `firmware/main/ui.c:304` — next_card_for() on page tap

### Outbound Dependencies
- `https://opencode.ai/workspace/wrk_01KT1W5K2X3FPZCZVFYYJWBEW8/usage` — OpenCode website API
- `~/.config/codexbar-toy/opencodego-history.json` — rolling history file
- `scripts/codexbar-stats.sh` — base payload with p/s/t bars
- `docs/generated/codexbar-payload.schema.json` — contract to update

### Infrastructure Wiring
- `scripts/codexbar-publish.sh:579-755` — cmd_once() sequential pipeline
- `scripts/codexbar-publish.sh:90-93` — Keychain token helpers
- `firmware/main/ui.c:1628` — ui_task stack at 8192 bytes
- `firmware/main/stats_model.c:310` — provider loop calls parse_cost/parse_pi/parse_lm/parse_cu/parse_ph (add parse_oc)

## Architecture Insights
- **Dedicated block pattern**: OpenCode Go follows the Cursor `cu` pattern (dedicated `oc` block, not generic `cost`) because the data source is separate from CodexBar (web API scraper vs local CLI)
- **In-place patch merge**: Since CodexBar already outputs the opencodego provider with p/s/t bars, the publisher merge patches the `oc` block onto it (like CURSOR_MERGE_JXA, not LM_MERGE_JXA)
- **Card lifecycle**: TODAY shows via `has_oc` (CARD_COST available), LIMITS via usage tiers (p/s/t from CodexBar). `provider_has_both_cards()` enables the 2-card toggle when `has_oc` is true
- **Fail-soft**: Publisher merge must never abort on scraper failure — if OpenCode website is unreachable, publish continues without the `oc` block (tokens/cost missing but p/s/t bars still work via CodexBar)
- **Auth cookie**: Same Keychain flow as Cursor session cookie — one-time paste-and-store, retrieved by scraper on each publish cycle

## Precedents & Lessons
3 similar past changes analyzed.

### Precedent: LM Studio stats provider (most similar)
**Commit(s)**: `dd123d2` — "feat: add LM Studio stats provider" (2026-05-25)
**Blast radius**: 10 files across 5 layers
  scripts/lmstudio-stats.sh — new host script
  scripts/codexbar-publish.sh — LM_MERGE_JXA + timeout-30 merge stage
  firmware/main/stats_model.h — lm_* dedicated fields
  firmware/main/stats_model.c — lm sub-object parse
  firmware/main/ui.c — provider_has_both_cards() + tap cycle
  docs/generated/codexbar-payload.schema.json — lm block

**Follow-up fixes** (5 within 6 days):
- `c79a46f` — icon scaling (2026-05-28)
- `280e632` — tap cycle stuck on STATS page (2026-05-27)
- `70efbf1` — chart-to-text overlap (2026-05-27)
- `20f42d3` — bottom text overlap with bar chart (2026-05-27)
- `d4f6f44` — icon replacement (2026-05-31)

**Takeaway**: Plan for at least one layout fixup. The `has_lm` flag had to be added to `provider_card_available()` to prevent tap cycle from getting stuck — same will apply for `has_oc`.

### Precedent: Pi Agent stats provider (canonical pattern)
**Commit(s)**: `f58693d` — "feat: add Pi Agent stats provider" (2026-05-22)
**Follow-up fixes**: `4613728` — timeout 30 added 15 min later

**Takeaway**: Publisher merge MUST have `timeout 30` + exit-code checking from day one.

### Precedent: Cursor token stats provider (in-place patch pattern)
**Commit(s)**: `392fa51` — "feat: add Cursor token stats provider" (2026-05-26)

**Takeaway**: In-place `cu` patch is simpler than LM Studio's full replace. OpenCode Go follows this pattern because base payload already has the provider (like Cursor).

### Composite Lessons
1. **Publisher merge must have `timeout 30` + fail-soft from day one** — Pi Agent needed this within 15 minutes (`4613728`). OpenCode Go scraper must follow same guard pattern.
2. **Card layout always needs visual fixup post-release** — LM Studio needed 3 layout fixes in 2 days. Plan for at least one layout fixup commit.
3. **Non-cost providers need explicit `has_*` boolean everywhere** — LM Studio's `has_lm` had to be added to `provider_card_available()`, `provider_has_both_cards()`, saver activity, and metric signing. Missing any one breaks tap cycle (`280e632`).
4. **Host scripts and firmware parser must stay in lockstep** — Every provider addition touched schema.json, stats_model.h/c, and host script in one commit.
5. **Auth cookie flow follows cursor pattern** — One-time paste via `--set-opencodego-cookie`, Keychain-stored, retrieved by scraper on each cycle.

## Developer Context
**Q (`scripts/codexbar-publish.sh:116-131`): The Cursor auth cookie flow — `store_cursor_session()` stores via `security add-generic-password` with `KC_SERVICE=codexbar-toy`, `KC_ACCOUNT=cursor-session`. Should OpenCode Go use the same service with a different account name (e.g., `opencodego-session`)?**
A: Yes — same `KC_SERVICE`, new `KC_ACCOUNT`. User confirmed: "use the same flow for saving auth cookies as we do for scraping cursor stats."

**Q (`ui_render.c:1143-1160`): The TODAY page has a mixed layout — token hero (like Cursor/LM Studio) + cost secondary line (like cost_standard). Should the `oc` block carry token AND cost history arrays, or just token history?**
A: Chart shows TOKENS only (single history array `ht[]`). Cost is a single today value `ct` (cents). Footer shows max tokens from history + today's cost as "max spend."

## Related Research
- `.rpiv/artifacts/research/2026-05-23_11-34-39_lm-studio-stats-on-device.md` — LM Studio research (most similar provider addition)
- `.rpiv/artifacts/research/2026-05-22_13-48-57_pi-agent-stats.md` — Pi Agent research (canonical provider addition pattern)
- `.rpiv/artifacts/research/2026-05-26_12-30-00_cursor-token-stats.md` — Cursor token stats research (in-place patch pattern)

## Open Questions
(none — all resolved during checkpoint)
