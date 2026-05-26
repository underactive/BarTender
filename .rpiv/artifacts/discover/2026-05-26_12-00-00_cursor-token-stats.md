---
date: 2026-05-26T12:00:00-0700
author: Eric Sison
commit: d1328cd
branch: master
repository: bartender
topic: "Cursor token stats on device"
tags: [intent, frd, firmware, scripts, cursor, codexbar-publish]
status: complete
last_updated: 2026-05-26T12:00:00-0700
last_updated_by: Eric Sison
---

# FRD: Cursor token stats on device

## Summary

Add a host-side Cursor token fetcher that calls Cursor's dashboard usage API with a Keychain-stored session cookie, rolls up paginated usage events into daily token totals over a rolling 30-day window, and merges a privacy-reduced `cu` sub-object onto the existing `cursor` provider in the Upstash payload. The firmware gains a real **CURSOR TODAY** page (LM Studio–style token hero + 30-day chart + max line) while CodexBar continues to supply Cursor **Limits** (`p` / `s` / `t`) unchanged.

## Problem & Intent

Eric wants the desk toy to match what he sees on [cursor.com/dashboard/usage](https://cursor.com/dashboard/usage) without manually checking the website. Success is trustworthy **"today"** and **30-day token aggregates** on the device — the same mental model as other providers — so Cursor token usage is visible at a glance alongside existing CodexBar limits data.

## Goals

- Fetch 30-day token usage from Cursor's dashboard API (`POST /api/dashboard/get-filtered-usage-events`) using a macOS Keychain–stored `WorkosCursorSessionToken`
- Persist a rolling 30-day local history of daily token totals on the Mac (host-side only)
- Merge token rollups into the published payload on the existing `id: "cursor"` provider via a new `cu` sub-object
- Enable a **CURSOR TODAY** firmware page: token hero, 30-day token bar chart, and 30-day max daily tokens (LM Studio TODAY layout; no dollar hero)
- Keep CodexBar-sourced Cursor **Limits** separate and unchanged

## Non-Goals

- Changing how Cursor limits (`p` / `s` / `t`) are sourced from CodexBar
- Headless browser scraping of the dashboard UI (API-first only)
- Publishing session tokens, cookies, per-event payloads, or account identity
- Cursor cost / spend tracking or a money hero on TODAY
- Real-time streaming; one fetch per publish cycle is sufficient
- Device-side history storage or credential management

## Functional Requirements

1. A new host script (`scripts/cursor-stats.sh` or equivalent) SHALL read `WorkosCursorSessionToken` from macOS Keychain and call `https://cursor.com/api/dashboard/get-filtered-usage-events` with JSON body `{ teamId, startDate, endDate, page, pageSize }` (epoch-ms date bounds, paginated).
2. The script SHALL paginate through all usage events for each required date window and sum per-event token field(s) into daily buckets using the **Mac local timezone** calendar day boundary (dashboard parity).
3. The script SHALL maintain a rolling 30-day history file under `~/.config/codexbar-toy/` (exact filename TBD in research) with daily token totals, pruned to 30 entries, updated each run.
4. The script SHALL emit a JSON provider fragment suitable for merge: `{ "id": "cursor", "ok": true, "cu": { "tk", "mxt", "ht" } }` where `tk` is tokens today, `mxt` is 30-day max daily tokens, and `ht` is daily token history (oldest → newest, up to 31 points).
5. `codexbar-publish.sh` SHALL gain a merge stage (mirroring `pi-agent-stats.sh` / `lmstudio-stats.sh`) that runs the Cursor stats helper and injects/replaces the `cu` block on the existing `cursor` provider without disturbing CodexBar limits fields.
6. `docs/generated/codexbar-payload.schema.json` SHALL document the optional `cu` sub-object on provider `id: "cursor"`.
7. The firmware SHALL parse `cu` fields into provider state and render **CURSOR TODAY** (`CARD_COST`) with LM Studio–style token layout: hero token count, 30-day token bar chart, and "30 DAY MAX" token label — no cost/dollar widgets.
8. When the API is unreachable, credentials are missing, or aggregation yields no usable data, the helper SHALL fail soft (`ok: false` or omit `cu`); the publisher SHALL log the skip and continue; Limits from CodexBar SHALL still display.

## Non-Functional Requirements

- **Performance**: Full 30-day backfill via paginated API should complete within one publish cycle (target ≤60s; exact budget set in research after response-size profiling).
- **Security**: `WorkosCursorSessionToken` stored in Keychain only; never committed, logged, or published. Only aggregate daily token integers cross the Upstash boundary. Per `docs/SECURITY.md`, credentials stay on the Mac.
- **UX**: CURSOR TODAY follows existing card navigation and Cursor accent (`0x00BFA5` in `firmware/main/provider_colors.h`). Limits page remains CodexBar-driven.
- **Reliability**: Handle expired session tokens gracefully (clear error, skip merge). Pagination must not silently truncate totals. History file writes must be atomic.

## Constraints & Assumptions

- Cursor token totals are **not** available from the current CodexBar/Cursor limits API; a separate dashboard API path is required.
- API contract reference (developer-provided): `POST https://cursor.com/api/dashboard/get-filtered-usage-events` with `teamId: 0` for personal usage; `startDate`/`endDate` as epoch milliseconds; `page` / `pageSize` for pagination.
- CodexBar already supplies the `cursor` provider entry with limits percentages; this work **extends** that entry rather than replacing it.
- Precedent: `scripts/lmstudio-stats.sh` + merge JXA in `scripts/codexbar-publish.sh:388-460` and LM TODAY rendering in `firmware/main/ui.c:1164-1199`.
- Publish cadence matches existing launchd schedule (~5 min).
- Exact token field name(s) in API event rows are unknown until research spikes the response JSON.

## Acceptance Criteria

- [ ] Running `./scripts/cursor-stats.sh` (name TBD) with a valid Keychain token prints JSON containing `id: "cursor"`, `ok: true`, and `cu.tk` / `cu.mxt` / `cu.ht[]` matching dashboard totals for today and the last 30 days (Mac local calendar days)
- [ ] The rolling history file under `~/.config/codexbar-toy/` is created/updated and pruned to ≤30 daily entries
- [ ] `./scripts/codexbar-publish.sh --once` merges `cu` onto the existing `cursor` provider while preserving CodexBar limits fields (`p`, `s`, `t`)
- [ ] `docs/generated/codexbar-payload.schema.json` includes the `cu` sub-object definition on the cursor provider
- [ ] Firmware shows **CURSOR TODAY** with token hero + 30-day chart + max label when `cu` is present; Limits page unchanged
- [ ] Missing/expired Keychain credential or API failure: publish continues, Limits still work, no secrets in logs or payload
- [ ] No session tokens, cookies, or per-event usage rows appear in git, Upstash payload, or serial logs

## Recommended Approach

A standalone host script `scripts/cursor-stats.sh` (shell wrapper + inline Python, mirroring `lmstudio-stats.sh`) that loads `WorkosCursorSessionToken` from Keychain, paginates `get-filtered-usage-events` over the rolling 30-day window, buckets token sums by Mac-local calendar day, read/writes `~/.config/codexbar-toy/cursor-history.json`, and emits a sanitized `cursor` provider fragment with a `cu` sub-object. Extend `codexbar-publish.sh` with a `CURSOR_MERGE_JXA` stage analogous to LM merge. Firmware: add `has_cu` / `cu_*` fields in `stats_model.*` and branch `ui.c` CARD_COST for `cursor` similar to `is_lmstudio` TODAY (token-only, no dollar hero).

## Decisions

### Primary beneficiary and success shape
**Question**: Who primarily benefits when this works, and what does "done" look like day to day?
**Recommended**: n/a — `intent` question
**Chosen**: Solo operator (option 1) — device matches cursor.com/dashboard/usage without opening the site; trustworthy "today" and 30-day token aggregates on the toy
**Rationale**: Developer framing: personal desk-toy telemetry parity with the Cursor dashboard

### Keep CodexBar limits separate
**Question**: Pre-resolved — CodexBar supplies Cursor limits (`p`/`s`/`t`) only; keep as-is and add token stats separately?
**Recommended**: Keep separate
**Chosen**: Keep separate
**Rationale**: evidence: `docs/generated/codexbar-payload.schema.json` has no Cursor token block; limits API lacks token totals + confirmed

### Enable real CURSOR TODAY (tokens)
**Question**: Pre-resolved — prior work had Cursor Limits only; goal is real CURSOR TODAY with tokens?
**Recommended**: Yes
**Chosen**: Yes
**Rationale**: Developer confirmed; aligns with "CURSOR TODAY stats on the device"

### Follow Pi/LM host-script + merge pattern
**Question**: Pre-resolved — follow dedicated host script + publisher merge + local 30-day history like Pi/LM Studio?
**Recommended**: Yes
**Chosen**: Yes
**Rationale**: evidence: `scripts/codexbar-publish.sh:59-60,388-460` + confirmed

### Separate dashboard API fetcher (not CodexBar-only)
**Question**: Pre-resolved — plan assumes new macOS fetcher for dashboard/API data, not extending CodexBar alone?
**Recommended**: Yes
**Chosen**: Yes
**Rationale**: Token totals unavailable from CodexBar limits path + confirmed

### Data source — HTTP API + session cookie
**Question**: How should the host fetch 30-day token data from Cursor?
**Recommended**: HTTP API + session cookie stored in Keychain (option 1)
**Chosen**: HTTP API + session cookie — `POST https://cursor.com/api/dashboard/get-filtered-usage-events` with `WorkosCursorSessionToken`
**Rationale**: Matches Pi/LM scheduled-publish pattern; developer supplied working curl as contract reference; optimizes reliability over scraping

### Payload shape — `cu` block, LM-style TODAY UI
**Question**: How should published token stats attach to the existing `cursor` provider?
**Recommended**: Optional `cu: { tk, mxt, ht }` on `id: "cursor"`; firmware TODAY branches like LM Studio (option 1)
**Chosen**: `cu` block, LM-style TODAY UI
**Rationale**: Keeps CodexBar limits untouched; token-first display without dollar hero; evidence: `firmware/main/ui.c:1164-1199` LM TODAY precedent

### Calendar day boundary — Mac local timezone
**Question**: Which day boundary for "today" and 30-day rollup?
**Recommended**: Mac local timezone (option 1)
**Chosen**: Mac local timezone
**Rationale**: Dashboard parity with cursor.com/dashboard/usage calendar picker; differs from Pi UTC (`scripts/pi-agent-stats.sh` UTC precedent) by explicit developer choice

### Aggregation — paginate events, sum token fields per row
**Question**: How to build daily token totals from `get-filtered-usage-events`?
**Recommended**: Paginate all pages; sum API per-event token fields into daily buckets; persist rolling 30 days locally (option 1)
**Chosen**: Paginate and sum per-event token fields
**Rationale**: Accurate when many events per day; avoids ~30 sequential day-window API calls

## Open Questions

- Exact JSON field name(s) on each usage event row that carry token counts (requires API response spike in research)
- Keychain service/account naming convention for `WorkosCursorSessionToken` (align with existing BarTender Keychain patterns if any)
- Whether `teamId: 0` is always correct for Eric's personal account or needs detection
- Final history filename and on-disk schema (e.g. `cursor-history.json` vs `cursor-token-history.json`)
- Whether summary-page token progress bar (analogous to LM Studio dual bars) is in scope for v1 or deferred to a follow-up

## Suggested Follow-ups

- Rotate the `WorkosCursorSessionToken` pasted in chat during discovery — treat as exposed before Keychain storage
- Summary-page Cursor token bar on the home screen (LM Studio shows dual stacked bars; interview scoped TODAY page only)
- Automatic Keychain token refresh / re-auth UX when the session expires
- Hourly or session-level token breakdown pages (beyond daily aggregates)

## References

- User input: Cursor token stats from dashboard usage page → local history → Upstash → device "CURSOR TODAY"
- API reference curl: `POST https://cursor.com/api/dashboard/get-filtered-usage-events` (developer-provided; do not commit cookies/tokens)
- `.rpiv/artifacts/discover/2026-05-23_10-41-20_lm-studio-stats.md` — LM Studio host/firmware precedent
- `.rpiv/artifacts/discover/2026-05-22_04-52-00_pi-agent-stats.md` — Pi Agent host/firmware precedent
- `scripts/lmstudio-stats.sh`, `scripts/codexbar-publish.sh`, `docs/generated/codexbar-payload.schema.json`, `docs/SECURITY.md`, `firmware/main/ui.c`
