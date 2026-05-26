---
date: 2026-05-23T11:34:39-0700
author: Eric Sison
commit: 5da5c88
branch: master
repository: bartender
topic: "LM Studio stats on device"
tags: [research, firmware, scripts, lm-studio, pipeline]
status: complete
last_updated: 2026-05-23T11:34:39-0700
last_updated_by: Eric Sison
---

# Research: LM Studio Stats on Device

## Research Question
Implement a new LM Studio provider in the BarTender pipeline that surfaces local LM Studio inference metrics — requests, tokens, model popularity, and cache utilization — from server logs at `~/.lmstudio/server-logs/`, following the Pi Agent provider precedent.

## Summary
The BarTender pipeline must add a new `lmstudio` provider with a `lm` sub-object. The host side requires a full rewrite of `scripts/lmstudio-stats.sh` to the `pi-agent-stats.sh` pattern (zsh wrapper + inline Python heredoc + rolling history file), a new `LM_MERGE_JXA` merge stage in `codexbar-publish.sh`, and the `lm` sub-object added within the existing v2 schema. The firmware needs dedicated `lm_*` fields on `stats_provider_t`, a new parse branch in `stats_model.c`, two new `card_kind_t` enum values (CARD_LM_STATS_2, CARD_LM_STATS_3) reusing CARD_COST/CARD_LIMITS slots for TODAY/STATS 1, and four LM Studio-specific rendering paths in `render_card()`. The provider color (`0x7C3AED`) and icon (`ic_lmstudio`) are already registered — no asset changes needed.

## Detailed Findings

### Host Script Rewrite (lmstudio-stats.sh)

#### Current state — needs full rewrite
- Shebang is `#!/usr/bin/env python3` (line 1) — does NOT follow the `pi-agent-stats.sh` pattern
- No zsh wrapper, no `PY="${PYTHON3:-$(command -v python3)}"` discovery
- No env overrides for log directory or history file
- `parse_all_logs()` (line 78) walks `~/.lmstudio/server-logs/{YYYY-MM}/{YYYY-MM-DD.N.log}` — correct directory structure, but no rolling history persistence
- `format_compact()` (line 100) produces a single-day snapshot — no 30-day window, no `h` or `ps`/`pt` equivalent output
- Output shape does NOT match the compact provider structure expected by `codexbar-publish.sh`

#### Required rewrite — follow pi-agent-stats.sh pattern
- `#!/bin/zsh` shebang, `PY="${PYTHON3:-$(command -v python3 2>/dev/null || true)}"` discovery (scripts/pi-agent-stats.sh:31)
- Inline Python heredoc: `"$PY" <<'PY'` (scripts/pi-agent-stats.sh:35)
- Env overrides: `LMSTUDIO_LOG_DIR`, `LMSTUDIO_HISTORY`, `PYTHON3` (matching `PI_AGENT_SESSIONS_DIR` at pi-agent-stats.sh:26)
- Rolling history file at `~/.config/codexbar-toy/lmstudio-history.json` (same config dir as publish config)
- Fail-soft: exits non-zero when no usable data (pi-agent-stats.sh:144, 161, 175, 179)
- Output shape: `{"id":"lmstudio","ok":true,"p":...,"lm":{"rq":...,"tk":...,"mxr":...,"mxt":...,"cp":...,"hr":[...],"ht":[...]}}`

#### Log directory structure
- `~/.lmstudio/server-logs/{YYYY-MM}/{YYYY-MM-DD.N.log}` — year-month subdirectories, daily files with `.N.log` rotation counter
- Multiple rotations per day observed (e.g., `2026-05-14.1.log` through `2026-05-14.49.log`)
- Old logs are rotated/deleted by LM Studio — history file is essential for 30-day persistence
- `parse_log()` (lmstudio-stats.sh:41) extracts 7 metric types via regex: `RE_REQUEST`, `RE_MODEL`, `RE_RELEASE`, `RE_CACHE`, `RE_CACHE_LOOKUP`, `RE_CACHE_FOUND`, `RE_PINNED`

#### Rolling history essential for 30-day persistence
- LM Studio rotates/archives old log files — a fresh parse cannot see rotated days
- History file persists aggregated per-day metrics across publisher cycles
- Updated every publish cycle: read → merge today → prune to 30 → write back → emit provider JSON
- Unlike `pi-agent-stats.sh` which reads immutable JSONL files with `rglob("*.jsonl")` (pi-agent-stats.sh:110)

### Publisher Merge (codexbar-publish.sh)

#### Current merge chain in cmd_once()
- `cmd_once()` (codexbar-publish.sh:386) runs sequentially: base payload → 4 JXA merge stages → Keychain token → curl POST
- All merge stages are fail-safe: non-zero exit logs a skip, never aborts the cycle
- **Stage 1** — `COST_MERGE_JXA` (line 418): Claude cost rollup from `~/Library/Caches/CodexBar/cost-usage/claude-v*.json`
- **Stage 2** — `CODEX_COST_MERGE_JXA` (line 426): Codex cost rollup from `pi-sessions-v*.json`
- **Stage 3** — `PCT_MERGE_JXA` (line 434): 24h usage-% sparkline from CodexBar history
- **Stage 4** — `PI_MERGE_JXA` (line 454): Pi Agent merge — the template pattern for LM Studio

#### PI_MERGE_JXA pattern (the template)
- Reads `CBPUB_JSON` (payload path) + `CBPUB_PI_JSON` (helper output path) — codexbar-publish.sh:353-354
- Validates payload shape: `pay.providers` must be an array (line 360)
- Validates helper shape: `src.id==='pi'`, `src.ok===true`, `src.pi` is an object (line 361)
- Extracts fields with safe `i32()`/`i64()` clamping (line 363)
- Builds provider object `{id:'pi', ok:true, pi:{ts,tt,ps,pt,h}}` (line 368)
- Replaces existing `pi` provider or prepends via `next.unshift(dst)` (lines 370-375)
- Writes back via `NSString.writeToFileAtomicallyEncodingError` (line 376-379)

#### Required changes for LM Studio merge
- New `LM_STATS` variable declaration near codexbar-publish.sh:59: `LM_STATS="${CBPUB_LM_STATS:-$SELF_DIR/lmstudio-stats.sh}"`
- New `LM_MERGE_JXA` heredoc (after PI_MERGE_JXA around line 383): validates `src.id==='lmstudio'`, `src.lm` sub-object with `rq/tk/mxr/mxt/cp/hr/ht` fields
- New `cmd_once()` block (after Pi block at line 464): `timeout 30 "$LM_STATS"` guard, fail-safe skip on timeout (124) or non-zero exit
- No changes to base payload generation, single-flight lock at `$lockdir`, or curl POST

### launchd Schedule
- `codexbar-publish.sh --install` (line 515) renders template at `launchd/com.codexbar-toy.publish.plist.template` → `~/Library/LaunchAgents/`
- Five placeholders: `__SCRIPT__`, `__PATH__`, `__CODEXBAR_BIN__`, `__INTERVAL__`, `__LOG__` (render_plist() at line 495)
- PATH is enriched with codex CLI + node bin dirs for launchd's sparse PATH (line 507-512)
- `PUBLISH_INTERVAL` default 300s, user config sets 60s (from `~/.config/codexbar-toy/config`)
- Rolling history file is a side effect of each publish cycle, not a daemon

### Firmware Data Model (stats_model.h / stats_model.c)

#### Current struct (stats_model.h:13-46)
- `stats_provider_t` has cost-shaped fields (`cost_today_c`, `tok_today`, `cost_month_c`, `tok_month`, `hist[]` with `hist_n`)
- Pi sub-object parsing at `stats_model.c:95-112` maps `ts→cost_today_c`, `tt→tok_today`, `ps→cost_month_c`, `pt→tok_month`, `h→hist[]`
- Comment at `stats_model.c:91-93` explains design rationale: "Reuse the shared cost-shaped fields so the UI can branch on provider id without forking the transport/model contract"

#### Required changes — dedicated `lm_*` fields
- Decision record (checkpoint): dedicated fields, NOT cost-shaped slot reuse
- New fields on `stats_provider_t`: `has_lm`, `lm_req_today` (int32), `lm_tok_today` (int64), `lm_req_month_max` (int32), `lm_tok_month_max` (int64), `lm_cache_pct` (float), `lm_hr[]` (int32, 31), `lm_ht[]` (int64, 31), `lm_hr_n`, `lm_ht_n`
- Plus top-10 model list and 7-day table fields (exact shape TBD at design time)
- Total addition ~420 bytes, bringing `stats_t` from ~1.9 KB to ~2.3 KB
- `memset(out, 0, sizeof *out)` at `stats_model.c:33` auto-zeroes new fields

#### Parse branch pattern
- New branch in `stats_model.c` (after pi block at line 112, before `ph` block at line 165):
```c
const cJSON *lm = cJSON_GetObjectItemCaseSensitive(e, "lm");
if (strcmp(p->id, "lmstudio") == 0 && cJSON_IsObject(lm)) {
    // Parse rq, tk, mxr, mxt, cp, hr[], ht[] with i32_clamp/i64_clamp
}
```

#### Payload schema updates (within v2)
- Decision record (checkpoint): add `lm` within v2 schema, no version bump
- Declare `lm` sub-object at `docs/generated/codexbar-payload.schema.json` alongside `pi`/`cost`/`ph`
- Fields: `rq` (integer), `tk` (integer), `mxr` (integer), `mxt` (integer), `cp` (number), `hr` (array of integer, maxItems:30), `ht` (array of integer, maxItems:30)
- `additionalProperties: false` on the `lm` sub-object
- No change to `"enum": [1, 2]` at schema line or `stats_model.c:65` version guard

### Firmware UI (ui.c)

#### Card navigation
- `card_kind_t` enum at `ui.c:32`: `{ CARD_COST, CARD_LIMITS, CARD_LM_STATS_2, CARD_LM_STATS_3 }`
- Decision record (checkpoint): reuse CARD_COST slot for TODAY, CARD_LIMITS slot for STATS 1, plus 2 new enums
- `is_lm` flag in `render_card()` (ui.c:1071) selects LM Studio content for CARD_COST/CARD_LIMITS
- Tap cycle at `ui.c:1621` changes from binary toggle to 4-card wrap-around for LM providers:
  - TODAY (CARD_COST with `is_lm`) → STATS 1 (CARD_LIMITS with `is_lm`) → STATS 2 (CARD_LM_STATS_2) → STATS 3 (CARD_LM_STATS_3) → TODAY

#### Summary row rendering
- Provider name at `ui.c:1412`: `lv_label_set_text(row_id[i], p->id)` — shows `"lmstudio"` by default
- Icon at `ui.c:1417-1425`: `provider_icon("lmstudio")` returns `&ic_lmstudio`, tinted to `0x7C3AED` via `prov_accent()` — both already registered
- Bar at `ui.c:1427-1445`: shows "off" text when `!p->has_p` — LM Studio needs a new field (e.g., `lm.p` or repurpose `p->p`) for the bar to render
- `is_hidden_provider()` at `ui.c:401-408`: `"lmstudio"` is NOT in HIDDEN_PROVIDERS — appears by default

#### Widget lifecycle
- `hide_cards()` at `ui.c:910` must hide all LM card panels plus cost/lim
- `hide_summary_chrome()` at `ui.c:925` stays unchanged (hides title/status/rows)
- `build_widgets()` at `ui.c:530` needs new LM-specific widget sets (hero labels, charts, tables)
- Each card panel is full-screen at W×H, positioned at (0,0) — only one visible at a time

#### Screensaver
- `saver_candidate_at()` at `ui.c:218-237` must add LM branch: if `is_lm(p->id)`, set `*card = CARD_COST` (for TODAY)
- `provider_card_available()` at `ui.c:205-207` must return true for LM card types
- `update_provider_activity_locked()` at `ui.c:192` must not filter out LM providers via `!p->has_cost && !provider_has_limits_card(p)` — add `|| p->has_lm`

### Precedents & Lessons
3 similar past changes analyzed.

### Precedent: Pi Agent stats provider
**Commit(s)**: `f58693d` — "feat: add Pi Agent stats provider" (2026-05-22)
**Blast radius**: 15 files across 6 layers
  scripts/pi-agent-stats.sh — new helper script
  scripts/codexbar-publish.sh — publisher merge wiring
  firmware/main/stats_model.h — new pi sub-object fields
  firmware/main/stats_model.c — pi field parsing
  firmware/main/ui.c — Pi-specific card rendering branch
  docs/generated/codexbar-payload.schema.json — pi provider block
  .rpiv/artifacts/ — design/discover/plan/research docs
  README.md, docs/EXTERNAL_INTEGRATIONS.md, docs/SECURITY.md — docs sync

**Follow-up fixes**:
- `4613728` — "fix: improve Pi Agent provider reliability and contract alignment" (15 min later) — timeout protection for Pi helper (timeout 30), schema required-fields fix
- `0c3a9ea` — "fix(ui): reuse autoscaled cost bar chart" (5h later) — chart autoscaling was missing for Pi's cost data path
- `16e4fd2` — "feat: show today's Pi Agent usage" (5h later) — follow-up feature

**Takeaway**: Publisher merge path needed immediate hardening (timeout, schema validation, fail-soft). Expect same for LM Studio.

### Precedent: OpenRouter stats
**Commit(s)**: `6595eb9` — "feat: OpenRouter stats on Today and Limits cards" (2026-05-20)
**Blast radius**: 5 files across 3 layers

**Follow-up fixes**:
- `78c9cfe` — "fix: OpenRouter Today layout + LEMONMILK hyphen glyph" (54 min later) — card layout wrong, missing U+002D in font range
- `8aead2c` — "fix: remove Codex today-cost date guard; limits card label + reset fixes" (2h later)

**Takeaway**: Card layout for new provider type needs visual review pass. For LM Studio's 4 custom pages, plan for at least one layout fixup commit.

### Precedent: Cursor 3-tier limits
**Commit(s)**: `c5e72d8` — "feat: Cursor 3-tier limits (Total/Auto/API), 1-decimal precision, today-cost date guard" (2026-05-20)

**Takeaway**: Today-cost date guards are tricky — had two fix iterations in one day. LM Studio has no cost/spend, so TODAY page must correctly skip cost fields entirely.

### Composite Lessons
1. **Publisher merge fail-soft is non-negotiable** — Pi Agent needed `timeout 30` guard and exit-code checking within 15 min of initial release. LM Studio publisher merge must be resilient to lmstudio-stats.sh timeout, malformed JSON, or server-not-running.
2. **Card layout for new provider always needs visual fixup** — OpenRouter TODAY hero/demote layout fixed 54 min later. LM Studio introduces 4 entirely new card types — plan for at least one layout/font/glyph fixup.
3. **Host scripts and firmware parser must stay in lockstep** — Every provider addition touched schema.json, stats_model.h/c, and the host script in the same commit.
4. **Font glyph gaps are recurring** — U+002D fix needed for OpenRouter. LM Studio tables use numbers, dates, % signs — verify all glyphs present in the LEMONMILK font range.
5. **No-cost provider needs explicit handling** — LM Studio has zero spend/cost. TODAY page must not show cost placeholders. The `has_balance` pattern is the right precedent.

### Historical Context (from `.rpiv/artifacts/`)
- `.rpiv/artifacts/discover/2026-05-23_10-41-20_lm-studio-stats.md` — Feature Requirements Document for LM Studio stats, all 11 decisions resolved
- `.rpiv/artifacts/plans/2026-05-22_14-23-07_pi-agent-stats.md` — Plan called out publisher merge must be fail-soft; follow-up fix `4613728` added exactly this guard
- `.rpiv/artifacts/research/2026-05-22_13-48-57_pi-agent-stats.md` — Research found provider_colors.h/icon registrations; LM Studio (#7C3AED) already registered
- `.rpiv/artifacts/designs/2026-05-22_14-16-27_pi-agent-stats.md` — Design called for 3 phases (host, UI, docs), executed sequentially

### Developer Context
**Q (`stats_model.h:13-19`): The Pi provider reuses cost-shaped slots. LM Studio has no spend, needs TWO parallel history arrays. Which struct approach?**
A: Dedicated `lm_*` fields (not cost-shaped slot reuse). Add `lm_req_today`, `lm_tok_today`, `lm_cache_pct`, `lm_hr[]`, `lm_ht[]` plus model/table fields.

**Q (`stats_model.c:65`): Should the `lm` sub-object bump to v3 or add within v2?**
A: Add within v2 (matching how `pi` was added in f58693d). Declare `lm` as optional property, no version guard change.

**Q (`ui.c:32`): CARD_COST/CARD_LIMITS binary enum — reuse slots or add 4 new for LM?**
A: Reuse CARD_COST for TODAY, CARD_LIMITS for STATS 1, plus 2 new enums (CARD_LM_STATS_2, CARD_LM_STATS_3).

## Code References
- `scripts/pi-agent-stats.sh:1` — zsh wrapper + inline Python heredoc pattern (template for lmstudio-stats.sh rewrite)
- `scripts/pi-agent-stats.sh:31` — Python discovery via env override or `command -v`
- `scripts/pi-agent-stats.sh:94-97` — env override defaults for testability
- `scripts/pi-agent-stats.sh:110` — `rglob("*.jsonl")` directory walk
- `scripts/pi-agent-stats.sh:170-171` — output shape: `{id,ok,p,pi:{ts,tt,ps,pt,h}}`
- `scripts/lmstudio-stats.sh:1` — current `#!/usr/bin/env python3` shebang (needs rewrite)
- `scripts/lmstudio-stats.sh:41-77` — `parse_log()` with 7 regex patterns
- `scripts/lmstudio-stats.sh:78-96` — `parse_all_logs()` directory walk
- `scripts/lmstudio-stats.sh:100-126` — `format_compact()` single-day output (needs full replacement)
- `scripts/codexbar-publish.sh:57-59` — STATS/PI_STATS variable declarations
- `scripts/codexbar-publish.sh:335-383` — PI_MERGE_JXA (template for LM_MERGE_JXA)
- `scripts/codexbar-publish.sh:396-398` — single-flight lock via mkdir
- `scripts/codexbar-publish.sh:438-443` — `-x "$PI_STATS"` guard + timeout 30
- `scripts/codexbar-publish.sh:495-513` — render_plist() PATH enrichment
- `firmware/main/stats_model.h:13-46` — stats_provider_t struct (needs lm_* fields)
- `firmware/main/stats_model.c:95-112` — pi sub-object parse (template for lm parse)
- `firmware/main/stats_model.c:65` — version guard (stays v1/v2)
- `firmware/main/fetch.c:44` — 4096-byte static body buffer
- `firmware/main/fetch.c:52` — stats_model_parse dispatch → ui_set_stats
- `firmware/main/ui.c:32` — card_kind_t enum (needs +2)
- `firmware/main/ui.c:1071-1240` — render_card() (needs is_lm branching)
- `firmware/main/ui.c:1621` — nav_card binary toggle (needs 4-card wrap for LM)
- `firmware/main/ui.c:218-237` — saver_candidate_at() (needs LM card handling)
- `firmware/main/ui.c:910-923` — hide_cards() (needs LM panel hiding)
- `firmware/main/provider_colors.h:46` — `"lmstudio": 0x7C3AED` (already registered)
- `firmware/main/provider_icons.c:1217` — `{"lmstudio", &ic_lmstudio}` (already registered)
- `docs/generated/codexbar-payload.schema.json:85-98` — pi sub-object schema (template for lm)

## Integration Points

### Inbound References
- `scripts/codexbar-publish.sh:59` — PI_STATS variable pattern, LM_STATS will follow
- `firmware/main/fetch.c:46-52` — do_fetch() calls upstash_get() → stats_model_parse() → ui_set_stats()
- `firmware/main/ui.c:1412-1445` — summary list renders each provider row

### Outbound Dependencies
- `~/.lmstudio/server-logs/` — LM Studio server log files (parsed fresh each cycle)
- `~/.config/codexbar-toy/lmstudio-history.json` — rolling history file (30-day persistence)
- `scripts/pi-agent-stats.sh` — precedent script pattern and env override conventions
- `docs/generated/codexbar-payload.schema.json` — payload contract to update

### Infrastructure Wiring
- `scripts/codexbar-publish.sh:386-475` — cmd_once() sequential pipeline
- `scripts/codexbar-publish.sh:515-523` — launchd install path
- `firmware/main/ui.c:1628` — ui_task stack at 8192 bytes
- `firmware/main/ui.c:1085-1087` — render_card() branching on is_pi, has_balance, and soon is_lm

## Architecture Insights
- **Provider sub-object pattern**: The `pi` sub-object pattern (provider-level sibling of `cost`/`ph`, parsed when `strcmp(p->id, "pi") == 0`) is the canonical approach for additive provider-specific fields
- **Cost-shaped slot reuse rule**: Pi reuses cost-shaped slots because its fields semantically match (cents + tokens + single history). LM Studio breaks this pattern (no cents, dual histories) — hence dedicated fields
- **Card slot reuse**: TODAY occupies CARD_COST slot, STATS 1 occupies CARD_LIMITS slot (by `is_lm` flag), STATS 2/3 are new enums. Minimizes enum churn while allowing LM-specific content in existing page slots
- **Fail-soft chain**: Every merge stage (including new LM_MERGE_JXA) must exit non-zero without aborting the publish cycle — the base payload ships without the provider
- **Rolling history as sole persistent state**: Unlike Pi Agent (immutable JSONL files), LM Studio logs rotate. The history file at `~/.config/codexbar-toy/lmstudio-history.json` is the only durable state for 30-day metrics

## Open Questions
(none — all resolved during checkpoint)

## Related Research
- `.rpiv/artifacts/research/2026-05-22_13-48-57_pi-agent-stats.md` — Pi Agent research (established pattern for provider sub-object, publisher merge, and card branching)