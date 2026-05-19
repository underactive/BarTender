# Tech Debt Tracker

Known technical debt, tracked as inventory. Items here should be addressed
by targeted cleanup tasks on a regular cadence — not accumulated for a
"big refactor."

## Format

```
### <short title>
- **Domain:** which domain is affected
- **Grade impact:** what quality grade this drags down
- **Severity:** low | medium | high
- **Added:** YYYY-MM-DD
- **Notes:** context for why this exists and what fixing looks like
```

## Active debt

### NVS encryption (risk elevated at payload v2)

- **Domain:** firmware
- **Grade impact:** firmware B+ → B
- **Severity:** medium
- **Added:** 2026-05-18
- **Notes:** Device NVS is unencrypted. At v2 the toy caches the owner's
  Claude spend + 30-day spend shape, so physical theft now leaks
  identity-adjacent financial data, not just "how busy." Fix: enable ESP-IDF
  flash/NVS encryption + secure boot. See `docs/SECURITY.md` §"Device boundary".

### CodexBar cost-cache format coupling

- **Domain:** codexbar-publish
- **Grade impact:** codexbar-publish B+ → B
- **Severity:** medium
- **Added:** 2026-05-18
- **Notes:** The Claude cost rollup parses CodexBar's **undocumented** internal
  cache (`~/Library/Caches/CodexBar/cost-usage/claude-v*.json`,
  `days[d][m]=[input,cacheRead,cacheCreate,output,costNanos,rows,rows]`). The
  filename version churns (`-v1/-v2/-v4/-v6` observed). Guarded: unknown shape
  → publish usage-only (fail-safe), but a silent CodexBar format change
  degrades the Cost card with no alert. Fix: pin/snapshot the schema in CI, or
  petition CodexBar for a stable `cost --json` CLI.

### No host test harness for the v2 parser/pipeline

- **Domain:** firmware / codexbar-publish
- **Grade impact:** firmware B
- **Severity:** low
- **Added:** 2026-05-18
- **Notes:** `stats_model_parse` (now v1‖v2 + cost block) and the publisher
  cost-merge JXA were verified manually against live data + sanitized
  fixtures, not in an automated harness. The `firmware/test/host/` seam should
  compile `stats_model.c` against host cJSON and assert the v2 fixture; the
  JXA merge needs a mock-cache test.

### Plist template lacks XML escaping (pre-existing)

- **Domain:** codexbar-publish
- **Grade impact:** none currently (paths are user-controlled, `plutil -lint`
  catches gross corruption)
- **Severity:** low
- **Added:** 2026-05-18 (surfaced by the claude-cost-menu audit; the code
  predates this change and was not modified by it)
- **Notes:** `render_plist()` substitutes `$LOG`/`$CODEXBAR_BIN`/`$PATH` into
  the launchd plist via `${t//__X__/$VAR}` with no XML entity escaping. A path
  containing `& < > " '` would produce a malformed plist. Fix: XML-escape the
  substituted values (or template via `plutil`/`PlistBuddy`). Tracked
  separately to avoid scope-creeping the cost-menu plan.

### 24h hourly cost not sourceable; per-model breakdown deferred

- **Domain:** firmware (Cost card)
- **Grade impact:** none (scoped deviation, documented)
- **Severity:** low
- **Added:** 2026-05-18
- **Notes:** CodexBar's cost cache is day-granular, so the Cost card's "24h"
  view degrades to a "today's spend" big number + the real 30-day daily bar
  chart (exec-plan `claude-cost-menu` Decision #3). Per-model (Opus/Sonnet/
  Haiku) `$` split exists in the cache `days` map but is intentionally not
  surfaced yet.

## Resolved debt

(none)

## Process

- When you discover tech debt during a task, add it here rather than fixing
  it inline (unless the fix is trivial and scoped to your current change).
- Cleanup tasks should reference the specific item they resolve.
- Move resolved items to the "Resolved" section with the date and PR/commit.
