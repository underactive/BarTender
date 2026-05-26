# BarTender

Pipe [CodexBar](https://github.com/steipete/CodexBar) AI-provider usage stats
and local Pi Agent usage rollups to an ESP32 desktop toy.

## Why

CodexBar shows your AI coding-plan usage in the macOS menu bar. This project
gets those numbers onto a standalone WiFi desk toy. The ESP32 can't reach the
Mac directly, so the pipeline is three stages:

1. **Read locally** — `scripts/codexbar-stats.sh` (done)
2. **Publish to a cloud key** — `scripts/codexbar-publish.sh` → Upstash (done)
3. **ESP32 reads + displays** it over WiFi — `firmware/` (done)

Everything macOS-side is zero-third-party-dependency beyond the local tools it
reads (`codexbar` and, when Pi is installed, Pi Agent state under `~/.pi/agent`),
plus base-macOS `osascript`/`curl`/`security`/`launchctl` and Python for local
JSON reduction.

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

# 3. (optional) Cursor token stats on the device (CURSOR TODAY page)
chmod +x scripts/cursor-stats.sh
./scripts/codexbar-publish.sh --set-cursor-session-clipboard   # after copying Cookie (see below)
./scripts/cursor-stats.sh                            # should print ok:true with cu:{...}
./scripts/codexbar-publish.sh --once                 # merges cu onto cursor limits row
```

**Cursor session (if `cursor-stats.sh` prints HTTP 403 or `no usable Cursor token data`):** log in at [cursor.com](https://cursor.com), open DevTools → **Network**, reload the usage dashboard, click any `cursor.com/api/…` request, and copy the full **`Cookie`** request header (not just `WorkosCursorSessionToken`). Then either:

- `./scripts/codexbar-publish.sh --set-cursor-session-clipboard` (copy first, then run — no typing), or
- `pbpaste | ./scripts/codexbar-publish.sh --set-cursor-session` (paste the Cookie line only, or the whole “Copy request headers” block — the script extracts the `Cookie:` line).

Diagnose without publishing secrets:

```bash
./scripts/cursor-stats.sh --check    # keychain / probe / api booleans
./scripts/cursor-stats.sh --debug    # adds stderr detail (cookie names, API shape)
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
| `codexbar-publish.sh --set-cursor-session` | Store Cursor Cookie header (visible terminal paste) |
| `codexbar-publish.sh --set-cursor-session-clipboard` | Store Cursor Cookie from clipboard (`pbpaste`) |
| `cursor-stats.sh` | Emit `cu` token rollup JSON (merged by the publisher onto `cursor`) |
| `codexbar-publish.sh --install` / `--uninstall` | Add/remove the launchd schedule |
| `codexbar-publish.sh --status` | Job state, target, token readiness, recent log |
| `codexbar-publish.sh --print-plist` | Preview the launchd plist that `--install` would write |

The **v2** payload carries usage % + reset hints + extra-usage $, and — for
Claude/Codex — total spend, token counts, and per-day spend history rolled up
from CodexBar's **local** cost caches. It can also append a first-class `pi`
provider from `scripts/pi-agent-stats.sh`, reduced from Pi Agent session JSONL
under `~/.pi/agent/sessions/` into max daily spend, max daily tokens, and a
30-day spend graph only. This is a **deliberately relaxed, private single-user
channel**: account email / identity are never projected, CodexBar's
per-project paths and Pi Agent raw sessions/prompts never leave the Mac, but
real spend now transits Upstash, so its endpoint + token must be kept private
(full rationale + residual NVS risk in [docs/SECURITY.md](docs/SECURITY.md)).
If there's no fresh data — or a local cache/Pi source is absent/format-churned
— the publish is skipped or falls back to the remaining reduced payload, so the
toy keeps its last good value. The write token lives in the Keychain; the ESP32
(Prompt 3) gets a separate read-only token.

### `firmware/` — ESP32-S3 desk toy

ESP-IDF firmware for the Freenove ESP32-S3 2.8" (FNK0104): joins WiFi, polls
the Upstash key over HTTPS every 5 min, renders the stats on the ILI9341.
A captive-portal handles setup so nothing secret is ever compiled in. It
remembers up to **5 WiFi networks** and autoconnects to whichever is in range
(home / work / café — no re-setup when you move); Upstash is stored
separately so changing WiFi never re-prompts for the token. On the summary
screen: **swipe up/down** to scroll the provider list, **tap a provider** to
open its **Cost** page, **tap again** to flip to **Usage Limits** (tapping
cycles the two; Claude/Codex show spend, tokens, and history; Pi shows today's
spend, today's tokens, 30-day maxes, and its 30-day graph), **swipe right→left** to go back. A
**long-press (~1.5 s)** adds a WiFi network
(non-destructive — keeps everything). Board bring-up is vendored from the
`clawd-tank` project.
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
