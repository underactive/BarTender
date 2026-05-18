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

(none)

## Resolved debt

(none)

## Process

- When you discover tech debt during a task, add it here rather than fixing
  it inline (unless the fix is trivial and scoped to your current change).
- Cleanup tasks should reference the specific item they resolve.
- Move resolved items to the "Resolved" section with the date and PR/commit.
