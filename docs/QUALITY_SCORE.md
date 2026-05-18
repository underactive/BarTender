# Quality Score

Tracks the current quality grade of each domain and architectural layer.
Updated as domains are built. Agents and humans use this to prioritize
cleanup and investment.

## Grading scale

| Grade | Meaning                                                    |
|-------|-----------------------------------------------------------|
| A     | Well-tested, documented, agent-legible, no known debt      |
| B     | Functional and tested, minor gaps in docs or edge cases    |
| C     | Works but has known debt, missing tests, or unclear naming |
| D     | Fragile, undertested, or structurally problematic          |
| F     | Broken or placeholder only                                 |

## Domain grades

| Domain            | Grade | Notes                                          | Last reviewed |
|-------------------|-------|-------------------------------------------------|---------------|
| <!-- e.g., `auth` --> | <!-- e.g., B+ --> | <!-- e.g., Works, needs integration tests --> | <!-- e.g., 2026-03-30 --> |

## Cross-cutting grades

| Concern           | Grade | Notes                                          | Last reviewed |
|-------------------|-------|-------------------------------------------------|---------------|
| <!-- e.g., Logging --> | <!-- e.g., B --> | <!-- e.g., Structured JSON, missing correlation IDs --> | <!-- e.g., 2026-03-30 --> |

## Process

- Review and update grades when a domain ships or changes significantly.
- A domain at grade C or below should have an entry in
  [tech-debt-tracker.md](exec-plans/tech-debt-tracker.md).
- Background cleanup tasks target the lowest-graded domains first.
