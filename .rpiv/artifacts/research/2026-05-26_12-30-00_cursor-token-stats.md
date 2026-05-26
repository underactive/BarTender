---
date: 2026-05-26T12:30:00-0700
author: Eric Sison
commit: d1328cd
branch: master
repository: bartender
topic: "Cursor token stats on device"
tags: [research, codebase, scripts, firmware, cursor, codexbar-publish]
status: complete
last_updated: 2026-05-26T12:30:00-0700
last_updated_by: Eric Sison
---

# Research: Cursor token stats on device

## Research Question

How should BarTender add Cursor dashboard token usage (30-day rollups + today) to the existing `cursor` provider and firmware CURSOR TODAY page, without disturbing CodexBar-sourced limits, following Pi/LM precedents?

Chained from: `.rpiv/artifacts/discover/2026-05-26_12-00-00_cursor-token-stats.md`

## Summary

Implement `scripts/cursor-stats.sh` (zsh + inline Python, mirroring `lmstudio-stats.sh`) that reads `WorkosCursorSessionToken` from Keychain, paginates `POST https://cursor.com/api/dashboard/get-filtered-usage-events`, buckets per-event token sums by **Mac local calendar day**, persists `~/.config/codexbar-toy/cursor-history.json`, and emits `{ "id": "cursor", "ok": true, "cu": { "tk", "mxt", "ht" } }`. On API/credential failure, follow **LM cached-history semantics** (developer chose Option A): if history has prior data, emit `ok: true` with last-good `cu` so TODAY/charts survive quiet days.

Extend `codexbar-publish.sh` with `CURSOR_MERGE_JXA` modeled on **`COST_MERGE_JXA`** (in-place patch of the existing `cursor` row), **not** `LM_MERGE_JXA` (full provider replace). Run helper under `timeout 30` like Pi/LM (`522-565`).

Firmware: add `has_cu` / `cu_*` parse branch in `stats_model.c` (parallel to `lm` at `168-238`), extend `provider_card_available` / CARD_COST gating (`ui.c:235-236`) to include `has_cu`, and branch CURSOR TODAY like LM token-only layout (`ui.c:1164-1199`) without requests hero or dollar widgets. Update `docs/generated/codexbar-payload.schema.json` with optional `cu` on `id: "cursor"`.

## Detailed Findings

### Host: new `cursor-stats.sh` (not in tree)

**Template:** `scripts/lmstudio-stats.sh` — zsh wrapper, `PY` discovery, env overrides, inline Python, rolling history under `~/.config/codexbar-toy/`, compact JSON stdout.

**API contract (from FRD):** `POST https://cursor.com/api/dashboard/get-filtered-usage-events` with JSON `{ teamId, startDate, endDate, page, pageSize }` (epoch-ms bounds). Paginate all pages for a single 30-day window; sum per-event token field(s) into daily buckets using **local timezone** `YYYY-MM-DD` keys (differs from Pi UTC in `scripts/pi-agent-stats.sh`).

**History file:** Recommend `~/.config/codexbar-toy/cursor-history.json` (matches FRD naming; same config dir as `lmstudio-history.json` at `lmstudio-stats.sh:68`). Schema: date string → `{ "tk": <int> }` per day. Prune: LM keeps **31** calendar keys (`275-278`) while FRD says ≤30 daily entries — implement **30 retained days** for Cursor to match acceptance criteria; emit `ht` oldest→newest, up to 31 points like LM `367-368` if chart needs an extra anchor day.

**`ok` flag (Option A — developer decision):** Mirror `lmstudio-stats.sh:344-347`: `ok = any_data or has_history`. When today's API fetch fails but history exists, still emit `ok: true` and `cu` from disk (stale `tk`/`mxt`/`ht` until next successful refresh). Missing Keychain token or empty history → `ok: false` or exit non-zero; publisher skips merge. Never log or print the session token.

**Keychain:** No Cursor-specific entry exists yet. Publisher uses `KC_SERVICE="${CBPUB_KC_SERVICE:-codexbar-toy}"` / `KC_ACCOUNT="publish"` for Upstash (`codexbar-publish.sh:53-54`). Cursor session should use a **separate** generic-password item (e.g. service `codexbar-toy`, account `cursor-session`, or dedicated `CBPUB_CURSOR_KC_*` env vars) — exact account string remains an open question; align with `docs/SECURITY.md` (credentials on Mac only).

**Performance:** FRD targets ≤60s for full pagination; publisher guard is **30s** (`549-550`). Design for incremental daily updates after first backfill; log timeout as skip, publish limits-only payload.

### Publisher: merge stage placement

**`cmd_once()` pipeline** (`463-569`): base `codexbar-stats.sh --json` → Claude cost → Codex cost → pct sparkline → Pi (`timeout 30`) → LM (`timeout 30`) → Upstash POST. All merges are fail-safe (log + continue).

**Correct merge pattern for Cursor:** `COST_MERGE_JXA` (`169-172`) iterates `pay.providers`, finds `pr.id==='claude'`, and patches `pr.cost` in place without removing CodexBar fields. Cursor must patch **`cu` on the existing `cursor` provider** the same way so `p`/`s`/`t` limits from CodexBar remain.

**Anti-pattern:** `LM_MERGE_JXA` (`445-454`) removes and re-inserts the entire `lmstudio` provider. Do **not** use this for Cursor — CodexBar already supplies `cursor` in the base payload (`stats_model.c:275` display order).

**New artifacts:** `CURSOR_STATS="${CBPUB_CURSOR_STATS:-$SELF_DIR/cursor-stats.sh}"`, `CURSOR_MERGE_JXA` heredoc validating `src.id==='cursor'`, `src.ok===true`, `src.cu` object with `tk` (i32), `mxt` (i64), `ht` (i64 array, cap 31). Loop `pay.providers` for `id==='cursor'`; assign `pr.cu = { tk, mxt, ht }`; exit 0 if no cursor row (nothing to merge). `cmd_once` block after LM merge with `timeout 30` and exit-code 124 handling.

### Payload schema

`docs/generated/codexbar-payload.schema.json` documents `pi` and `lm` sub-objects; **no `cu` yet**. Add optional `cu` on providers where `id` is `cursor`, fields `tk` (today tokens), `mxt` (30-day max daily tokens), `ht[]` (daily token totals, oldest→newest). Producer line in schema `description` should mention `cursor-stats.sh`.

### Firmware: parse + UI

**Parse:** `stats_model.c:168-238` shows the `lm` block pattern under `strcmp(p->id, "lmstudio")`. Add parallel branch for `strcmp(p->id, "cursor")` + `cu` object: `cu_tok_today`, `cu_tok_month_max`, `cu_ht[]`, `cu_ht_n`, `has_cu`.

**CARD_COST availability:** `ui.c:235` returns `p->has_cost || p->has_lm` only — Cursor with limits but no cost will not show TODAY until extended to `|| p->has_cu`.

**CURSOR TODAY layout:** Copy LM token-only branch (`1164-1199`): hero `fmt_tokens` on `cu_tok_today`, hide dollar/cost/OR rows, 30-day bar chart from `cu_ht`, caption `30 DAY MAX` from `cu_tok_month_max`. **Omit** LM's secondary requests hero (`cost_tok` / `lm_req_today`). Accent already `0x00BFA5` in `provider_colors.h:16`. Icon `ic_cursor` registered in `provider_icons.c:1207`.

**Limits card:** Unchanged — CodexBar `p`/`s`/`t` on `cursor`; no `has_lm` coupling.

**Summary page:** LM shows dual stacked bars when `has_lm` (`ui.c:1289+`). FRD defers Cursor summary token bar to follow-up — v1 is CARD_COST only.

### CodexBar base payload

`cursor` is a first-class provider in firmware display order (`stats_model.c:275`). Limits come from CodexBar projection in `scripts/codexbar-stats.sh` (not re-read here). Token totals are **not** in that path — confirmed by FRD and absence of `cu` in schema.

## Code References

- `scripts/codexbar-publish.sh:169-172` — in-place `cost` patch on existing `claude` provider (Cursor `cu` merge template)
- `scripts/codexbar-publish.sh:388-460` — `LM_MERGE_JXA` full provider replace (do not copy for Cursor)
- `scripts/codexbar-publish.sh:522-565` — Pi/LM `timeout 30` helper invocation pattern for Cursor stage
- `scripts/lmstudio-stats.sh:68` — history path under `~/.config/codexbar-toy/`
- `scripts/lmstudio-stats.sh:275-278` — 31-day history prune (Cursor should use 30 per FRD)
- `scripts/lmstudio-stats.sh:344-347` — `ok` from history when today has no fresh data (Option A)
- `scripts/codexbar-publish.sh:53-54` — Keychain service naming for Upstash (precedent for Cursor credential storage)
- `firmware/main/stats_model.c:168-238` — `lm` sub-object parse template for `cu`
- `firmware/main/stats_model.h:66-76` — `has_lm` / `lm_*` fields to mirror with `cu_*`
- `firmware/main/ui.c:235-236` — CARD_COST gating (must add `has_cu`)
- `firmware/main/ui.c:1164-1199` — LM TODAY token hero + chart (CURSOR TODAY template)
- `firmware/main/provider_colors.h:16` — Cursor accent `0x00BFA5`
- `firmware/main/stats_model.c:275` — `cursor` in display order
- `docs/generated/codexbar-payload.schema.json:17` — provider `id` examples include cursor; no `cu` yet
- `docs/SECURITY.md:36-60` — publish boundary; credentials stay on Mac

## Integration Points

### Inbound References

- `scripts/codexbar-publish.sh:463-569` — `cmd_once` will invoke `cursor-stats.sh` and `CURSOR_MERGE_JXA` after LM merge
- `firmware/main/stats_model.c` — JSON consumer for Upstash payload
- `firmware/main/ui.c` — CARD_COST / CARD_LIMITS rendering for `cursor`

### Outbound Dependencies

- Cursor dashboard API `get-filtered-usage-events` (external HTTPS)
- macOS Keychain for `WorkosCursorSessionToken`
- `~/.config/codexbar-toy/cursor-history.json` (host-only persistence)
- Base payload from `scripts/codexbar-stats.sh` (existing `cursor` limits row)

### Infrastructure Wiring

- `launchd/com.codexbar-toy.publish.plist.template` — unchanged schedule; new merge runs inside existing publish cycle
- Upstash POST — same path after all merges (`571-578`)
- No firmware credential or history storage

## Architecture Insights

1. **Two merge families:** *Patch sub-object on existing provider* (Claude/Codex `cost`, future Cursor `cu`) vs *Replace entire provider* (Pi, LM). Cursor is firmly in the patch family.
2. **Two TODAY families:** *Cost/dollar hero* (Claude/Codex/OpenRouter) vs *Token-first* (LM). Cursor TODAY is token-first, single hero (no requests line).
3. **Fail-soft everywhere:** Helper non-zero, JXA non-zero, or timeout → log `note:` and publish without `cu`; limits still flow from CodexBar.
4. **Cached aggregates (Option A):** Stale token display beats blank TODAY when API/session fails — matches operator mental model for a desk toy.
5. **Timezone choice is load-bearing:** Mac local days for Cursor; do not reuse Pi UTC bucketing.

## Precedents & Lessons

8+ related commits across Pi/LM/cost merges and Cursor limits (per scope-tracer / git sweep).

### Precedent: Pi Agent provider + merge
**Commit(s):** `f58693d` area — Pi `pi-agent-stats.sh` + `PI_MERGE_JXA` + firmware `pi` block  
**Blast radius:** scripts + publish JXA + `stats_model` + `ui.c`  
**Lessons:** Ship `timeout 30` and fail-soft merge on day one; sanitize helper output in JXA before POST.

### Precedent: LM Studio stats
**Commit(s):** `dd123d2`, `51a359e` — `lmstudio-stats.sh` rewrite + `LM_MERGE_JXA` + LM TODAY UI  
**Blast radius:** same layers as Pi  
**Lessons:** Rolling history file is required when upstream data rotates; `ok` from history prevents blank device days.

### Precedent: Cursor limits only
**Commit(s):** `c5e72d8` — CodexBar `cursor` limits on device without token block  
**Lesson:** Extend `cursor` row; never replace or drop limits fields.

### Composite Lessons

- Always pair host helper + JXA merge + schema + firmware parse/UI in one feature (`f58693d`, `51a359e` pattern).
- Use `COST_MERGE_JXA` patch semantics when CodexBar already owns the provider id (`169-172`).
- Match publisher timeout to helper complexity; FRD 60s API budget may require pagination tuning inside 30s guard or a follow-up timeout bump.

## Historical Context (from `.rpiv/artifacts/`)

- `.rpiv/artifacts/discover/2026-05-26_12-00-00_cursor-token-stats.md` — FRD, decisions, acceptance criteria
- `.rpiv/artifacts/discover/2026-05-23_10-41-20_lm-studio-stats.md` — LM Studio discover precedent
- `.rpiv/artifacts/discover/2026-05-22_04-52-00_pi-agent-stats.md` — Pi Agent discover precedent
- `.rpiv/artifacts/research/2026-05-23_11-34-39_lm-studio-stats-on-device.md` — LM implementation research
- `.rpiv/artifacts/research/2026-05-22_13-48-57_pi-agent-stats.md` — Pi implementation research

## Developer Context

**Q (discover: Primary beneficiary and success shape): Who primarily benefits when this works, and what does "done" look like day to day?**  
A: Solo operator — device matches cursor.com/dashboard/usage without opening the site; trustworthy "today" and 30-day token aggregates on the toy.

**Q (discover: Keep CodexBar limits separate): Pre-resolved — keep limits as-is and add token stats separately?**  
A: Keep separate.

**Q (discover: Enable real CURSOR TODAY (tokens)): Goal is real CURSOR TODAY with tokens?**  
A: Yes.

**Q (discover: Follow Pi/LM host-script + merge pattern): Follow dedicated host script + publisher merge + local 30-day history?**  
A: Yes.

**Q (discover: Separate dashboard API fetcher): New macOS fetcher for dashboard API, not CodexBar-only?**  
A: Yes — `POST get-filtered-usage-events` with Keychain session cookie.

**Q (discover: Payload shape — `cu` block, LM-style TODAY UI): How should token stats attach to `cursor`?**  
A: Optional `cu: { tk, mxt, ht }`; firmware TODAY like LM Studio token layout (no dollar hero).

**Q (discover: Calendar day boundary): Which day boundary for rollup?**  
A: Mac local timezone (dashboard parity).

**Q (discover: Aggregation): How to build daily totals?**  
A: Paginate all pages; sum per-event token fields into daily buckets; persist rolling 30 days locally.

**Q (`scripts/lmstudio-stats.sh:344-347`): When dashboard API fails but `cursor-history.json` has data, emit cached `cu` or omit?**  
A: **Option A — Cached `cu`:** API down/expired session → `ok: true` + last good `cu` from history; CodexBar limits unchanged.

## Related Research

- `.rpiv/artifacts/research/2026-05-23_11-34-39_lm-studio-stats-on-device.md`
- `.rpiv/artifacts/research/2026-05-22_13-48-57_pi-agent-stats.md`

## Open Questions

- Exact JSON field name(s) on each usage event row for token counts (requires live API response spike)
- Keychain service/account naming for `WorkosCursorSessionToken` (separate from Upstash `publish` account)
- Whether `teamId: 0` is always correct for personal usage or needs detection
- Final on-disk history schema details beyond `cursor-history.json` date → `{tk}` map
- Whether summary-page Cursor token bar (LM dual-bar analog) is in scope for v1 or deferred
- Publisher `timeout 30` vs FRD ≤60s API budget — tune pagination or raise timeout after profiling
