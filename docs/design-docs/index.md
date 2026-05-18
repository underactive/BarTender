# Design Docs Index

Design docs capture significant architectural decisions and their rationale.
Each doc should be self-contained enough for an agent to understand the
decision without external context.

## Format

```
docs/design-docs/YYYY-MM-DD-<slug>.md
```

Each design doc should include:
- **Status:** Draft | Active | Superseded
- **Problem:** What we're solving and why
- **Decision:** What we chose
- **Alternatives considered:** What else we evaluated
- **Consequences:** What this decision enables and constrains

## Active design docs

| Doc | Status | Summary |
|-----|--------|---------|
| [core-beliefs.md](core-beliefs.md) | Active | Agent-first operating principles |

## Verification

Design docs are considered verified when the described architecture matches
the actual codebase. Stale docs should be updated or marked Superseded.
