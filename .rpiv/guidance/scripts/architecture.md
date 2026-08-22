# Scripts Layer

## Responsibility
Host-side executable tooling for the macOS half of the pipeline: read CodexBar usage locally, publish a normalized payload to Upstash, generate firmware icon assets, and speak the firmware screenshot protocol from a desktop client.

## Dependencies
- **macOS CLI tools (`osascript`, `security`, `launchctl`)**: shape JSON processing, secret storage, and scheduled execution patterns
- **CodexBar local files + CLI**: source of provider usage and cost-cache inputs
- **Python + Pillow / pyserial / `rsvg-convert`**: asset generation and host-device tooling shapes

## Consumers
- **`README.md` users**: run `codexbar-stats.sh` and `codexbar-publish.sh` directly
- **launchd**: executes `codexbar-publish.sh --once` on schedule
- **Firmware**: consumes generated `provider_icons.*`, published Upstash payloads, and the screenshot host protocol peer

## Module Structure
```text
scripts/
├── codexbar-stats.sh, codexbar-publish.sh   # User-facing pipeline CLIs
├── cursor-stats.sh, pi-agent-stats.sh, …    # Publish merge helpers
├── preflight.sh, uninstall.sh
├── lib/_stats_history.py                    # Shared Python (stats scripts)
└── build/
    ├── gen-provider-icons.py, gen-boot-splash.py
    ├── screenshot.py, boot-capture.sh
    └── assets/codexbar-logos/             # SVG inputs for icon generation
```

## CLI Script Dispatcher (Header Docs + `cmd_*` Functions)
```zsh
#!/bin/zsh
set -u

log()  { print -r -- "$(date '+%Y-%m-%dT%H:%M:%S%z') $*"; }
die()  { log "ERROR: $*"; exit "${2:-1}"; }
help() { awk 'NR>=2 && /^#/{sub(/^# ?/,"");print;next} NR>=2{exit}' "$0"; }

cmd_once()    { read_config; run_publish_cycle; }
cmd_install() { read_config; render_plist > "$PLIST"; }
cmd_status()  { read_config; print -r -- "config=$CFG"; }

case "${1:---once}" in
  --once|"") cmd_once ;;
  --install)  cmd_install ;;
  --status)   cmd_status ;;
  -h|--help)  help ;;
  *)          print -r -- "unknown argument: $1" >&2; exit 2 ;;
esac
```

## Shell Orchestration + Embedded JXA (Structured JSON Without `jq`)
```zsh
read -r -d '' MERGE_JXA <<'EOF'
ObjC.import('Foundation');
function rf(path) { /* read UTF-8 text, return null on failure */ }
var payload = JSON.parse(rf($.getenv('PAYLOAD_JSON')));
var extra = JSON.parse(rf($.getenv('EXTRA_JSON')));
if (!Array.isArray(payload.providers)) $.exit(2); // fail closed on shape drift
for (const p of payload.providers) {
  if (p.id === 'claude') {
    p.cost = p.cost || {};          // additive merge, not replace-wholesale
    p.cost.history = extra.history || [];
  }
}
EOF

base_payload >"$payload"
extra_payload >"$extra"
PAYLOAD_JSON="$payload" EXTRA_JSON="$extra" osascript -l JavaScript -e "$MERGE_JXA"
```

## Secure Publisher Boundary (Keychain + Lockdir + Launchd Template)
```zsh
get_token() {
  security find-generic-password -s "$KC_SERVICE" -a "$KC_ACCOUNT" -w 2>/dev/null
}

cmd_once() {
  lockdir="$LOG_DIR/.publish.lock"
  # mkdir is atomic; record the owner so a dead publisher can be recovered.
  # Preserve a live PID lock. Legacy ownerless locks are age-gated before removal.
  acquire_lock "$lockdir" || exit 0
  trap 'rm -rf "${work:-}"; release_lock' EXIT INT TERM

  build_payload >"$payload" || exit 3; // skip instead of blanking remote state
  tok="$(get_token)" || exit 5
  ( umask 077; print -r -- "header = \"Authorization: Bearer $tok\"" >"$curlcfg" )
  curl -sS -K "$curlcfg" --data-binary @"$payload" "$UPSTASH_URL"
}
```

## Generator / Protocol Pairing (Emit Artifacts, Then Consume Stable Interfaces)
```python
# gen-provider-icons.py
ID_TO_ASSET = {"claude": "claude", "codex": "codex"}
rgba = rasterize_svg(path)
alpha = list(rgba.split()[-1].tobytes())   # emit A8 mask; firmware applies color later
write_generated_c_and_h(alpha_tables)

# screenshot.py
ser.write(b"screenshot\n")                 # text command
scan_for_magic(ser, b"SCAP")               # recover from serial log noise
w, h, data_len = struct.unpack("<HHI", ser.read(8))
```

## Credit-balance projection

The JXA projection in `codexbar-stats.sh --json` accepts a provider with only
CodexBar generic `credits.remaining` as usable data, then emits that dollar
balance as integer-cent `cost.cr`. Moonshot currently exposes `Balance: $…` in
its login-method display string and DeepSeek exposes `$…` at the beginning of
its reset display string; the projection extracts only those dollar amounts
into `cost.cr`. OpenRouter's dedicated balance remains the preferred source.
This preserves the reduced, firmware-owned payload rather than exposing
upstream provider objects.

## OpenRouter token rollup

CodexBar reports OpenRouter in dollars only, so `openrouter-stats.sh` pulls
tokens directly from `POST /api/v1/analytics/query` and `merge-or.js` folds
`cost.tt` + `cost.ht` into the block the projection already built. The query
uses **hour** granularity and re-buckets into local calendar days: the daily
`/api/v1/activity` endpoint refuses the in-progress UTC day, which would
otherwise violate the local-date rollup rule below. Tokens go in the generic
`cost` block rather than a provider-specific one because `cost.tt` already
existed and only the token history (`cost.ht`, sibling of the `cost.h` spend
history) was missing.

## DeepSeek token + spend rollup

CodexBar's DeepSeek hook returns a balance and no usage, so `deepseek-stats.sh`
pulls tokens and spend from the console's private dashboard endpoints
(`GET /api/v0/usage/{amount,cost}`) and `merge-ds.js` folds `cost.tt`, `ht`,
`ct` and `cw` into the block the projection already built.

Four things constrain this integration:

- **The merge must be additive.** The projection has already reduced DeepSeek's
  balance display string into `cost.cr`, and that field is what makes
  `ui_render_card.c` choose the balance layout. Replacing the `cost` object
  instead of merging into it collapses the card to the standard layout, which
  charts fields DeepSeek does not publish.
- **Auth failures arrive as HTTP 200** with `code`/`data.biz_code` `40002` or
  `40003`. A status-only check reads `data:null` as an empty month and
  publishes zeros, so the code check is mandatory and is evaluated before the
  generic non-zero-code branch to keep the "token expired" message actionable.
- **The endpoints are month-scoped**, so a 30-day window fetches the current
  and previous month and stitches them on absolute date keys. No local history
  cache: the window is fully re-derivable each run.
- **The local-date rollup rule below cannot be honoured here.** Days arrive
  pre-bucketed as `YYYY-MM-DD` with no timezone and no hourly granularity to
  re-bucket from, unlike OpenRouter where hour buckets made local days
  possible. This is a documented, deliberate exception — we match the API's
  day string to the local date rather than guessing an offset correction. The
  one correction made is folding future-dated buckets into today: DeepSeek's
  timezone is ahead of US local time either way, so afternoon/evening usage is
  filed under tomorrow and would otherwise be dropped for ~15 hours.

`cost.h` is deliberately not published: the balance renderer charts `tok_hist`
and never reads `p->hist`, and the device's response buffer is a fixed size
shared by every provider, so an unrendered field is pure overflow risk.

## Pi-derived Moonshot / Qwen Cloud tokens

Neither provider exposes token usage anywhere: Moonshot's API is balance-only
and Qwen Cloud bills in Credits. Pi Agent session rows carry
`message.provider` beside per-turn token counts, so `pi-agent-stats.sh` emits a
`derived[]` array in the same scan it already performs, and `merge-pd.js` folds
it into those providers' `cost` blocks.

This is an accepted undercount, not an estimate to be refined. It captures only
Pi-routed traffic; on the DeepSeek control (the sole provider with a real
dashboard total to compare against) Pi saw 41% of August tokens and nothing at
all on 12 of 21 active days. Anything that reconciles these figures against a
balance or a provider console will disagree, by design.

Three rules keep it honest:

- **Match the provider id, not the model.** `kimi-*` and `qwen-*` also run via
  `opencode-go` and `ramp-router`, which bill separately and are already their
  own providers; matching on model name would double-count across providers.
- **Publish only what the target card renders.** Moonshot has a balance so it
  draws the balance card (`tt` + `ht` chart); Qwen Cloud has none so it draws
  the standard card, which charts spend rather than `ht` and surfaces a 30-day
  token total (`tt` + `tm`). Publishing `ht` for Qwen would render nothing.
- **Do not derive spend for Moonshot.** Its card shows the true account balance
  one row below; an undercounted SPEND next to an accurate balance contradicts
  itself on screen. Tokens have no competing authoritative number on the card.

Everything `pi-agent-stats.sh` emits — Pi's own totals and these slices — keys
on the local calendar day, per the rule below. It bucketed in UTC until
2026-08-21; besides rolling Pi's "today" over mid-afternoon, each UTC bucket
straddled two local days, which inflated the 30-day token peak and split single
spend days across two buckets.

## Architectural Boundaries
- **NO secrets in committed config or argv**: write tokens live in Keychain, not files or plist args
- **NO raw upstream payload publishing**: scripts project/merge a reduced contract before Upstash
- **Today rollups use the local system date**: never infer "today" from the newest upstream cache key; a missing current-day key publishes zero while structural cache failures still skip publishing.
- **NO manual edits to generated firmware assets**: regenerate `provider_icons.*` from source SVGs
- **NO cross-import assumptions**: these are process-level tools and protocol peers, not a shared library

<important if="you are adding a new command or mode to a shell script in this layer">
## Shell Script Checklist
1. Document the command in the file header so `--help` stays self-derived.
2. Add a `cmd_*` function and wire it into the final `case` dispatcher.
3. Parse config manually from `KEY=VALUE` files; never `source` app config.
4. Reuse shared helpers for logging, temp dirs, and cleanup.
5. Preserve skip/fail-safe behavior when partial data is acceptable.
</important>

<important if="you are extending published JSON or CodexBar-derived fields in this layer">
## Payload Extension Checklist
1. Keep shell responsible for orchestration and subprocess flow.
2. Put structured JSON parsing/merge logic in embedded JXA, not fragile shell string munging.
3. Validate source JSON shape before reading fields.
4. Merge only the derived fields needed by firmware.
5. Preserve the “skip rather than overwrite good remote data with bad data” rule.
</important>

<important if="you are adding a new provider icon or branding asset in this layer">
## Icon Generation Checklist
1. Add the SVG under `scripts/build/assets/codexbar-logos/`.
2. Update the generator’s provider-id → asset mapping.
3. Re-run `scripts/build/gen-provider-icons.py` to regenerate `firmware/main/provider_icons.c` and `.h`.
4. Add matching color data in `firmware/main/provider_colors.h` if the provider is new.
5. Never hand-edit the generated C icon tables.
</important>

<important if="you are adding a new host↔device tool command or protocol in this layer">
## Host/Device Protocol Checklist
1. Define the text trigger and framed response on both sides together.
2. Add a magic/header if binary data shares the serial/log channel.
3. Keep endian and payload-length rules explicit in both implementations.
4. Update the firmware peer and desktop client in the same change.
5. Document the wire format near both endpoints.
</important>
