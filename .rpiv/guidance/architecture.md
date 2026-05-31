# Project Overview
BarTender pipes local CodexBar AI-provider usage stats through a macOS publishing script into an ESP32-S3 desk toy. The repo is split between host-side scripts, ESP-IDF firmware, and a strong documentation/spec layer.

# Architecture
- `firmware/` - ESP-IDF project for the desk toy runtime
- `scripts/` - macOS host pipeline, asset generation, and host-device utilities
- `docs/` - product specs, plans, reliability/security rules, generated payload schema
- `launchd/` - LaunchAgent template used by the publisher

Flow: `codexbar-stats.sh` reads local data → `codexbar-publish.sh` projects and publishes JSON to Upstash → `firmware/main/` fetches, parses, and renders it.

For layer-specific guidance, see:
- `.rpiv/guidance/firmware/architecture.md`
- `.rpiv/guidance/firmware/main/architecture.md`
- `.rpiv/guidance/scripts/architecture.md`

# Commands
| Command | What it does |
|---|---|
| `./scripts/codexbar-stats.sh` | Print local CodexBar usage report |
| `./scripts/codexbar-stats.sh --json` | Emit compact payload consumed by the publisher |
| `./scripts/codexbar-publish.sh --once` | Run one Upstash publish cycle |
| `./scripts/codexbar-publish.sh --install` | Install the launchd schedule |
| `cd firmware && idf.py build` | Build the ESP32 firmware |
| `cd firmware && idf.py flash monitor` | Flash firmware and open serial monitor |
| `python3 scripts/build/gen-provider-icons.py` | Regenerate firmware provider icons from SVG assets |
| `python3 scripts/build/screenshot.py` | Capture a PNG via the firmware screenshot protocol |

# Business Context
This is a private single-user telemetry display: upstream account identity and per-project paths stay local, while the device receives a reduced payload containing usage and spend summaries suitable for display.

<important if="you are changing the end-to-end payload shape between the host scripts and the firmware">
- Treat `docs/generated/codexbar-payload.schema.json` as the contract to update.
- Change the producer in `scripts/` and the consumer in `firmware/main/stats_model.*` together.
- Keep projection/privacy guarantees intact: do not start publishing raw CodexBar data.
- Update `README.md` and relevant product specs when user-visible fields or behavior change.
</important>

<important if="you are modifying security-sensitive config, tokens, or local captured data">
- Read `docs/SECURITY.md` before changing storage or publish behavior.
- Write tokens belong in Keychain on macOS and read-only credentials belong on the device; do not hardcode secrets.
- Files under `docs/references/*.sample.json` are local/private fixtures and must stay out of git per `.gitignore`.
</important>

<important if="you are implementing a change that spans host scripts and firmware">
1. Update the host-side producer in `.rpiv/guidance/scripts/architecture.md`.
2. Update firmware fetch/parse/render logic in `.rpiv/guidance/firmware/main/architecture.md`.
3. Reflect contract changes in `docs/generated/codexbar-payload.schema.json`.
4. Update the relevant spec under `docs/product-specs/` and any README usage text.
</important>
