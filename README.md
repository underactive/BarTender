# codexbarjar

Pipe [CodexBar](https://github.com/steipete/CodexBar) AI-provider usage stats
to an ESP32 desktop toy.

## Why

CodexBar shows your AI coding-plan usage in the macOS menu bar. This project
gets those numbers onto a standalone WiFi desk toy. The ESP32 can't reach the
Mac directly, so the pipeline is three stages:

1. **Read locally** — `scripts/codexbar-stats.sh` (done)
2. **Publish to a cloud key** — `scripts/codexbar-publish.sh` → Upstash (done)
3. **ESP32 reads + displays** it over WiFi — `firmware/` (done)

Everything macOS-side is zero-third-party-dependency (only `codexbar` itself,
plus base-macOS `osascript`/`curl`/`security`/`launchctl`).

## Installation

- macOS with [CodexBar](https://github.com/steipete/CodexBar) installed and at
  least one provider enabled (`~/.codexbar/config.json`).
- No build step. Scripts are self-contained zsh.

## Quick start

```sh
# 1. See your stats in the terminal (~2s)
./scripts/codexbar-stats.sh

# 2. (Prompt 2) publish to Upstash on a schedule
#    a. create an Upstash Redis DB; copy its REST URL + a write token
mkdir -p ~/.config/codexbar-toy && chmod 700 ~/.config/codexbar-toy
printf 'UPSTASH_REST_URL=https://<db>.upstash.io\nUPSTASH_KEY=codexbar\n' \
  > ~/.config/codexbar-toy/config
./scripts/codexbar-publish.sh --set-token      # paste the WRITE token
./scripts/codexbar-publish.sh --install        # launchd publishes every 5 min
```

## Usage

### `scripts/codexbar-stats.sh` — local report

| Command | Effect |
|---------|--------|
| `codexbar-stats.sh` | Detailed report for enabled providers (~2s, parallel) |
| `codexbar-stats.sh --json` | Compact **v2** JSON for the publisher (usage % + extra-usage $ in cents; PII never projected) |
| `codexbar-stats.sh --all` | Every provider via one slow `--provider all` call (~90s; debug) |
| `codexbar-stats.sh --help` | Full options + env vars |

### `scripts/codexbar-publish.sh` — Upstash publisher

| Command | Effect |
|---------|--------|
| `codexbar-publish.sh --once` | One publish cycle (default) |
| `codexbar-publish.sh --set-token` | Store the Upstash write token in the macOS Keychain |
| `codexbar-publish.sh --install` / `--uninstall` | Add/remove the launchd schedule |
| `codexbar-publish.sh --status` | Job state, target, token readiness, recent log |
| `codexbar-publish.sh --print-plist` | Preview the launchd plist that `--install` would write |

The **v2** payload carries usage % + reset hints + extra-usage $, and — for
Claude — total spend (today / 30-day), token counts, and a 30-day per-day
spend history rolled up from CodexBar's **local** cost cache. This is a
**deliberately relaxed, private single-user channel**: account email /
identity are never projected, and CodexBar's per-project paths never leave the
Mac, but real spend now transits Upstash, so its endpoint + token must be kept
private (full rationale + residual NVS risk in
[docs/SECURITY.md](docs/SECURITY.md)). If there's no fresh data — or the cost
cache is absent/format-churned — the publish is skipped or falls back to
usage-only, so the toy keeps its last good value. The write token lives in the
Keychain; the ESP32 (Prompt 3) gets a separate read-only token.

### `firmware/` — ESP32-S3 desk toy

ESP-IDF firmware for the Freenove ESP32-S3 2.8" (FNK0104): joins WiFi, polls
the Upstash key over HTTPS every 5 min, renders the stats on the ILI9341.
A captive-portal handles setup so nothing secret is ever compiled in. It
remembers up to **5 WiFi networks** and autoconnects to whichever is in range
(home / work / café — no re-setup when you move); Upstash is stored
separately so changing WiFi never re-prompts for the token. On the summary
screen: tap to refresh, triple-tap to add a WiFi network (non-destructive —
keeps everything). **Swipe down** to
open a provider menu → tap a provider → **Cost** / **Usage Limits** cards
(Claude shows real spend, tokens, and a 30-day history chart); **swipe up**
backs out one level. Board bring-up is vendored from the `clawd-tank` project.
Build/flash/provisioning: [firmware/README.md](firmware/README.md). Full
behavior: [docs/product-specs/claude-cost-menu.md](docs/product-specs/claude-cost-menu.md).

## Architecture

See [ARCHITECTURE.md](ARCHITECTURE.md). Integration details (CodexBar, Upstash)
in [docs/EXTERNAL_INTEGRATIONS.md](docs/EXTERNAL_INTEGRATIONS.md); the active
plan in [docs/exec-plans/active/](docs/exec-plans/active/).

## Contributing

See [AGENTS.md](AGENTS.md) for repo conventions, workflow, and orientation.

## License

Not yet specified.
