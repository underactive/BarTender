# Core Beliefs

These are this project's agent-first operating principles. They define how we
build and how agents should reason about the codebase.

## 1. Repository is the system of record

Every decision, spec, and convention lives in the repo. If it's not
discoverable by an agent reading the repo, it doesn't exist. Slack threads,
Google Docs, and conversations are ephemeral — commit the decision or it rots.

## 2. Agents execute, humans steer

Human time is the scarcest resource. Optimize for leverage: invest human
attention in system design, boundary definitions, and feedback loops — not in
writing boilerplate or fixing lint errors. When an agent struggles, ask "what
capability or context is missing?" rather than "let me just do it manually."

## 3. Parse at the boundary, trust the interior

All external data (API responses, CLI input, file contents, third-party
output) is validated and structured at entry. Interior code operates on
typed, validated objects and does not re-check. This keeps the codebase lean
and makes agent-generated code safer.

## 4. Enforce mechanically, not manually

If a rule matters, encode it as a lint rule, structural test, or CI check.
Documentation states intent; tooling enforces it. Rules that rely on humans
remembering them will be violated by agents (and humans) on the first busy day.

## 5. Prefer boring technology

Choose dependencies and patterns that are composable, stable, and
well-represented in agent training data. Boring tech is easier for agents to
reason about. When a third-party package is opaque or over-featured, prefer
reimplementing the subset you need with full test coverage.

## 6. Progressive disclosure over front-loading

AGENTS.md is a map, not a manual. Agents start with a small entry point and
follow links to deeper context as needed. This respects context windows and
keeps agents focused on the task at hand.

## 7. Garbage-collect continuously

Technical debt compounds. Bad patterns replicate because agents copy what
exists. Run cleanup tasks on a regular cadence — small, targeted refactoring
PRs are cheaper than periodic rewrites. Capture taste as golden principles and
enforce them automatically.

## 8. Optimize for agent legibility

Code, docs, and architecture are optimized for the next agent run, not human
aesthetic preferences. If output is correct, maintainable, and legible to
future agent runs, it meets the bar — even if a human would have formatted it
differently.

## 9. Plans are artifacts, not conversations

Complex work is captured in execution plans checked into the repo with progress
logs and decision records. Plans are versioned, reviewable, and discoverable
by agents. Ephemeral plans are fine for small changes; complex work gets a
proper exec plan.
