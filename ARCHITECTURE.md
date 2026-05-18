# {{PROJECT_NAME}} Architecture

## System overview

<!-- Draw your system's high-level component diagram here.
     Show major components, their relationships, and data flow direction.
     ASCII art works well for agent legibility. Example:

     ┌─────────────┐     ┌─────────────┐
     │   CLI / UI   │────▶│   Service    │
     └─────────────┘     └──────┬──────┘
                                │
                                ▼
                         ┌─────────────┐
                         │  Data Layer  │
                         └─────────────┘
-->

## Domain layers

<!-- Define your dependency layers. The key principle: dependency flows in
     one direction only. Adapt these layers to your architecture. -->

| Layer        | Responsibility                                    | May depend on        |
|--------------|---------------------------------------------------|----------------------|
| **Types**    | Shared interfaces, enums, schemas                 | Nothing              |
| **Config**   | Environment, feature flags, configuration         | Types                |
| **Repository** | Data access, file I/O, persistence              | Types, Config        |
| **Service**  | Business logic, orchestration, parsing            | Types, Config, Repo  |
| **Runtime**  | CLI entry points, HTTP handlers, process lifecycle | All above            |
| **UI**       | Terminal output, formatting, interactive prompts  | All above            |

Cross-cutting concerns (logging, telemetry, error handling) should be injected
via a shared interface. Domains should not import cross-cutting code directly.

## Domains

<!-- List every bounded domain in your system. Update this table when adding
     or removing domains. -->

| Domain            | Purpose                                           | Status      |
|-------------------|---------------------------------------------------|-------------|
| <!-- e.g., `auth` --> | <!-- e.g., Authentication and session management --> | <!-- e.g., Implemented --> |

## Key design decisions

<!-- Record architectural decisions that agents need to understand.
     Focus on "why" — agents can read the code to learn "what".
     Number them sequentially and never remove entries (mark superseded instead). -->

1. <!-- e.g., **Adapter pattern for external services.** We use a uniform adapter
      interface for all third-party integrations so they can be swapped or mocked
      independently. -->
