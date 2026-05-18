# Plans Index

Plans are first-class artifacts in this project. Complex work gets an execution
plan checked into the repo. Small changes use ephemeral plans (inline in the
PR description or a task tool).

## Active plans

| Plan | Goal | Owner | Started |
|------|------|-------|---------|
| [codexbar-publish](exec-plans/active/codexbar-publish.md) | Publish minimal CodexBar usage JSON to Upstash via launchd (Prompt 2 of 3) | — | 2026-05-17 |

See [exec-plans/active/](exec-plans/active/) for full plan documents.

## Completed plans

| Plan | Goal | Completed |
|------|------|-----------|
| (none) | — | — |

See [exec-plans/completed/](exec-plans/completed/) for full plan documents.

## Creating a plan

Use an execution plan when the work:
- Touches 3+ domains or files
- Requires multiple sequential steps with dependencies
- Involves a non-obvious architectural decision
- Will take more than one session to complete

### Plan template

```markdown
# Plan: <title>

- **Started:** YYYY-MM-DD
- **Status:** Active | Blocked | Completed
- **Objective** — what is being implemented and why. one sentence.
- **Changes** — files to modify/create, with descriptions of each change
- **Dependencies** — any prerequisites or ordering constraints between changes
- **Risks / open questions** — anything flagged during planning that needs attention

## Context
Why this work is needed.

## Steps
- [ ] Step 1
- [ ] Step 2

## Decisions
- YYYY-MM-DD: Decided X because Y.

## Open questions
- Question?
```

Save to `docs/exec-plans/active/<slug>.md`. Move to `completed/` when done.
