# Product Specs Index

Product specs define user-facing behavior and acceptance criteria. Agents
reference these to understand what the product should do, not just how the
code is structured.

## Core user flow

<!-- Describe your application's core user flow in numbered steps. Example:

1. User provides input (file, URL, command)
2. System processes and validates input
3. System performs core operation
4. Output is structured and enriched
5. User reviews results
6. User takes action based on results -->

## Specs

| Spec | Status | Description |
|------|--------|-------------|
| [publish-to-upstash](publish-to-upstash.md) | Implemented (live round-trip verified) | Periodic non-sensitive usage publish to Upstash via launchd |
| [esp32-toy](esp32-toy.md) | Code complete (pending user build+flash) | ESP32-S3 device: reads Upstash, renders stats, captive provisioning |

## Writing specs

Each spec should define:
- **User story:** Who wants what and why
- **Acceptance criteria:** Observable behaviors that must be true
- **Edge cases:** What happens when input is invalid, services fail, or timeouts occur
- **Not in scope:** Explicitly state what this spec does NOT cover
