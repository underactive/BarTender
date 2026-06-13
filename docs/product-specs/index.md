# Product Specs Index

Product specs define user-facing behavior and acceptance criteria. Agents
reference these to understand what the product should do, not just how the
code is structured.

## Core user flow

1. User configures CodexBar on macOS and runs `./scripts/preflight.sh --install` to wire up the launchd publisher.
2. `codexbar-stats.sh` reads each enabled provider's usage locally and emits a compact v2 JSON payload.
3. `codexbar-publish.sh` rolls up cost/tokens from CodexBar's local cost cache, merges optional Pi Agent / Cursor / LM Studio rollups, and POSTs the v2 JSON to the private Upstash key every 5 minutes.
4. The ESP32-S3 desk toy polls Upstash over HTTPS every 60 s, parses the payload, and renders one row per provider on the summary screen.
5. The user taps a provider row to see its Cost or Usage Limits page; long-press opens the non-destructive add-network portal when relocating between remembered WiFis.

## Specs

| Spec | Status | Description |
|------|--------|-------------|
| [publish-to-upstash](publish-to-upstash.md) | Implemented (live round-trip verified) | Periodic non-sensitive usage publish to Upstash via launchd |
| [esp32-toy](esp32-toy.md) | Code complete (pending user build+flash) | ESP32-S3 device: reads Upstash, renders stats, captive provisioning |
| [claude-cost-menu](claude-cost-menu.md) | Code complete, build-clean (pending user flash) | Scrollable summary + tap-cycle Claude Cost/Usage-Limits pages; swipe-left back; long-press re-provision |

## Writing specs

Each spec should define:
- **User story:** Who wants what and why
- **Acceptance criteria:** Observable behaviors that must be true
- **Edge cases:** What happens when input is invalid, services fail, or timeouts occur
- **Not in scope:** Explicitly state what this spec does NOT cover
