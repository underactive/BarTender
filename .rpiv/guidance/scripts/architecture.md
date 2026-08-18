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

## Architectural Boundaries
- **NO secrets in committed config or argv**: write tokens live in Keychain, not files or plist args
- **NO raw upstream payload publishing**: scripts project/merge a reduced contract before Upstash
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
