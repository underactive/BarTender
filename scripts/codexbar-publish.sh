#!/bin/zsh
# codexbar-publish.sh — POC (Prompt 2 of 3): publish a minimal, non-sensitive
# CodexBar usage JSON to Upstash Redis (REST) on a schedule, so the future
# ESP32 toy (Prompt 3) can read it with a read-only token.
#
#   codexbar-publish.sh                 # one publish cycle (same as --once)
#   codexbar-publish.sh --once
#   codexbar-publish.sh --set-token     # store the Upstash WRITE token (Keychain)
#   codexbar-publish.sh --set-cursor-session  # paste Cookie header (terminal; Cmd+V works)
#   codexbar-publish.sh --set-cursor-session-clipboard  # read Cookie from pbpaste
#   codexbar-publish.sh --set-opencodego-cookie  # paste OpenCode Go Cookie header
#   codexbar-publish.sh --set-opencodego-cookie-clipboard  # read from pbpaste
#   codexbar-publish.sh --install       # install + start the launchd schedule
#   codexbar-publish.sh --uninstall     # stop + remove the launchd schedule
#   codexbar-publish.sh --status        # job state + recent log + readiness
#   codexbar-publish.sh --print-plist   # render the launchd plist to stdout
#   codexbar-publish.sh --help
#
# Security model (see docs/SECURITY.md — RELAXED for a private single-user
# channel; privacy now rests on Upstash endpoint + token secrecy):
#   - The payload = `codexbar-stats.sh --json` (usage % + reset hints +
#     extra-usage $ as cents) PLUS Claude and Codex `cost` blocks (today/30d
#     $ + tokens + per-day history) rolled up from CodexBar's LOCAL cost cache.
#   - The cost cache's `files` map (private project paths) is NEVER read; only
#     the aggregate `days` map is, and only rolled-up numbers are forwarded.
#   - Account email / identity / loginMethod are still never projected.
#   - Write token lives in the macOS Keychain, never in argv/log/plist.
#   - curl auth header is passed via a 0600 temp -K config, not the command
#     line (no secret in `ps`).
#   - If there is no fresh data, the publish is SKIPPED (the store keeps its
#     last good value — a transient local failure must not blank the toy).
#   - A single-flight lock prevents overlapping cycles.
#
# Non-secret config: ~/.config/codexbar-toy/config  (KEY=VALUE lines)
#   UPSTASH_REST_URL=https://<db>.upstash.io
#   UPSTASH_KEY=codexbar            # Redis key (default: codexbar)
#   PUBLISH_INTERVAL=300            # launchd seconds (default: 300)
#   MOCK_SINK_URL=                  # test override; if set, used instead of Upstash
#
# Test hooks (env, default to real user paths — never modify CodexBar state):
#   CBPUB_COST_CACHE_DIR  CodexBar cost-usage dir
#                         (default: ~/Library/Caches/CodexBar/cost-usage)
#   CBPUB_PCT_HISTORY     CodexBar hourly usage-% history file (default:
#                         ~/Library/Application Support/com.steipete.codexbar/
#                         history/claude.json)
#   (Codex cost reads pi-sessions-v*.json from the same CBPUB_COST_CACHE_DIR)
#   CBPUB_PI_STATS       Pi Agent reducer helper (default: sibling pi-agent-stats.sh)
#   PI_AGENT_HOME / PI_AGENT_SESSIONS_DIR / PI_AGENT_MODELS_FILE
#                         forwarded to pi-agent-stats.sh for hermetic tests
#
# Zero third-party deps: codexbar-stats.sh + base-macOS security/curl/launchctl/
# osascript/awk/date/mktemp + zsh builtins.
set -u

LABEL="com.codexbar-toy.publish"
# Test overrides (hermetic verification; default to real user paths):
KC_SERVICE="${CBPUB_KC_SERVICE:-codexbar-toy}"; KC_ACCOUNT="publish"
CURSOR_KC_ACCOUNT="${CBPUB_CURSOR_KC_ACCOUNT:-cursor-session}"
CFG="${CBPUB_CONFIG:-$HOME/.config/codexbar-toy/config}"; CFG_DIR="${CFG:h}"
LOG_DIR="${CBPUB_LOG_DIR:-$HOME/Library/Logs/codexbar-toy}"; LOG="$LOG_DIR/publish.log"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
SELF_DIR="${0:A:h}"
STATS="$SELF_DIR/codexbar-stats.sh"
PI_STATS="${CBPUB_PI_STATS:-$SELF_DIR/pi-agent-stats.sh}"
LM_STATS="${CBPUB_LM_STATS:-$SELF_DIR/lmstudio-stats.sh}"
CURSOR_STATS="${CBPUB_CURSOR_STATS:-$SELF_DIR/cursor-stats.sh}"
OPENCODE_GO_STATS="${CBPUB_OPENCODE_GO_STATS:-$SELF_DIR/opencodego-stats.sh}"
KC_ACCOUNT_OG="${CBPUB_KC_ACCOUNT_OG:-opencodego-session}"
work=""   # temp dir for cmd_once; referenced by its global EXIT trap
TPL="$SELF_DIR/../launchd/$LABEL.plist.template"

log()  { print -r -- "$(date '+%Y-%m-%dT%H:%M:%S%z') $*"; }
die()  { log "ERROR: $*"; exit "${2:-1}"; }
help() { awk 'NR>=2 && /^#/{sub(/^# ?/,"");print;next} NR>=2{exit}' "$0"; }

# Parse known KEY=VALUE pairs from $CFG without eval/source (SECURITY.md).
UPSTASH_REST_URL=""; UPSTASH_KEY="codexbar"; PUBLISH_INTERVAL="300"; MOCK_SINK_URL=""
read_config() {
  [[ -r "$CFG" ]] || return 0
  local line k v
  while IFS= read -r line || [[ -n "$line" ]]; do
    line="${line%%#*}"; [[ "$line" == *=* ]] || continue
    k="${${line%%=*}// /}"; v="${line#*=}"; v="${v## }"; v="${v%% }"
    v="${v#[\"\']}"; v="${v%[\"\']}"
    case "$k" in
      UPSTASH_REST_URL) UPSTASH_REST_URL="$v" ;;
      UPSTASH_KEY)      UPSTASH_KEY="${v:-codexbar}" ;;
      PUBLISH_INTERVAL) PUBLISH_INTERVAL="${v:-300}" ;;
      MOCK_SINK_URL)    MOCK_SINK_URL="$v" ;;
    esac
  done < "$CFG"
}

get_token() { security find-generic-password -s "$KC_SERVICE" -a "$KC_ACCOUNT" -w 2>/dev/null; }

get_cursor_session() {
  security find-generic-password -s "$KC_SERVICE" -a "$CURSOR_KC_ACCOUNT" -w 2>/dev/null
}

cmd_set_token() {
  # `security` prompts for the password ONLY when -w is the LAST option (per
  # its own usage text: "Specify -w as the last option to be prompted").
  # The token is typed hidden + retyped; it never enters argv/log.
  print -r -- "Storing Upstash WRITE token in Keychain (service=$KC_SERVICE)."
  print -r -- "macOS 'security' will prompt: paste the token, Enter, then retype."
  if ! security add-generic-password -U -s "$KC_SERVICE" -a "$KC_ACCOUNT" \
        -l "CodexBar toy publish token" -w; then
    die "security add-generic-password failed (token NOT stored)"
  fi
  # Guard: the prompt is skippable — verify a non-empty secret actually landed.
  # (This is the exact bug that previously reported success on an empty item.)
  local n; n=$(get_token | wc -c | tr -d ' ')
  [[ "${n:-0}" -ge 2 ]] || die "no token captured (empty) — re-run --set-token and paste the token at the prompt" 5
  log "token stored in Keychain (${n} bytes incl. trailing newline)"
}

# Cursor cookies are long and must be pasted from DevTools; macOS `security -w`
# alone uses a hidden Keychain prompt that often blocks Cmd+V. Read in-terminal
# (visible) or from clipboard, then write Keychain with -w <value>.
store_cursor_session() {
  local session="$1"
  session="${session//$'\r'/}"
  # DevTools "Copy request headers" is multiline; keep the Cookie line, not :method/POST.
  if [[ "$session" == *$'\n'* ]] || [[ "$session" != *"="* ]]; then
    local extracted
    extracted=$(print -r -- "$session" | awk '
      BEGIN { IGNORECASE=1 }
      /^cookie:[[:space:]]*/ {
        sub(/^[^:]*:[[:space:]]*/, "")
        print
        exit
      }
    ')
    [[ -n "$extracted" ]] && session="$extracted"
  fi
  session="${session%%$'\n'*}"
  session="${session## }"; session="${session%% }"
  [[ -n "$session" ]] || die "empty session — nothing stored" 5
  if ! security add-generic-password -U -s "$KC_SERVICE" -a "$CURSOR_KC_ACCOUNT" \
        -l "CodexBar toy Cursor session" -w "$session"; then
    die "security add-generic-password failed (session NOT stored)"
  fi
  local n; n=$(get_cursor_session | wc -c | tr -d ' ')
  [[ "${n:-0}" -ge 2 ]] || die "no session captured (empty) — re-run --set-cursor-session" 5
  log "Cursor session stored in Keychain (${n} bytes incl. trailing newline)"
}

cmd_set_cursor_session() {
  print -r -- "Storing Cursor session cookie in Keychain"
  print -r -- "(service=$KC_SERVICE account=$CURSOR_KC_ACCOUNT)."
  print -r -- ""
  print -r -- "Copy from DevTools → Network → any cursor.com/api request →"
  print -r -- "Request Headers → Cookie (full string), then paste below."
  print -r -- ""
  print -r -- "Or: copy first, then run: $0 --set-cursor-session-clipboard"
  print -r -- ""
  local session
  if [[ ! -t 0 ]]; then
    session=$(cat)
    print -r -- "(read from stdin)"
  else
    print -r -- "Paste Cookie header, then Enter (visible; Cmd+V works):"
    read -r session
  fi
  store_cursor_session "$session"
}

cmd_set_cursor_session_clipboard() {
  command -v pbpaste >/dev/null 2>&1 || die "pbpaste not found (macOS only)"
  local session; session=$(pbpaste)
  [[ -n "${session//[$'\r\n\t ']}" ]] || die "clipboard empty — copy Cookie header first" 5
  store_cursor_session "$session"
}

# ── OpenCode Go session helpers (mirrors Cursor keychain pattern) ────────
get_opencodego_session() {
  security find-generic-password -s "$KC_SERVICE" -a "$KC_ACCOUNT_OG" -w 2>/dev/null
}

store_opencodego_session() {
  local session="$1"
  session="${session//$'\r'/}"
  if [[ "$session" == *$'\n'* ]] || [[ "$session" != *"="* ]]; then
    local extracted
    extracted=$(print -r -- "$session" | awk '
      BEGIN { IGNORECASE=1 }
      /^cookie:[[:space:]]*/ {
        sub(/^[^:]*:[[:space:]]*/, "")
        print
        exit
      }
    ')
    [[ -n "$extracted" ]] && session="$extracted"
  fi
  session="${session%%$'\n'*}"
  session="${session## }"; session="${session%% }"
  [[ -n "$session" ]] || die "empty session — nothing stored" 5
  if ! security add-generic-password -U -s "$KC_SERVICE" -a "$KC_ACCOUNT_OG" \
        -l "CodexBar toy OpenCode Go session" -w "$session"; then
    die "security add-generic-password failed (session NOT stored)"
  fi
  local n; n=$(get_opencodego_session | wc -c | tr -d ' ')
  [[ "${n:-0}" -ge 2 ]] || die "no session captured (empty) — re-run --set-opencodego-cookie" 5
  log "OpenCode Go session stored in Keychain (${n} bytes incl. trailing newline)"
}

cmd_set_opencodego_cookie() {
  print -r -- "Storing OpenCode Go session cookie in Keychain"
  print -r -- "(service=$KC_SERVICE account=$KC_ACCOUNT_OG)."
  print -r -- ""
  print -r -- "Copy from DevTools → Network → opencode.ai API request →"
  print -r -- "Request Headers → Cookie (full string), then paste below."
  print -r -- ""
  print -r -- "Or: copy first, then run: $0 --set-opencodego-cookie-clipboard"
  print -r -- ""
  local session
  if [[ ! -t 0 ]]; then
    session=$(cat)
    print -r -- "(read from stdin)"
  else
    print -r -- "Paste Cookie header, then Enter (visible; Cmd+V works):"
    read -r session
  fi
  store_opencodego_session "$session"
}

cmd_set_opencodego_cookie_clipboard() {
  command -v pbpaste >/dev/null 2>&1 || die "pbpaste not found (macOS only)"
  local session; session=$(pbpaste)
  [[ -n "${session//[$'\r\n\t ']}" ]] || die "clipboard empty — copy Cookie header first" 5
  store_opencodego_session "$session"
}

# Roll up Claude total spend / tokens / 30-day history from CodexBar's LOCAL
# cost cache and merge into the v2 payload. Reads ONLY the aggregate `days`
# map (date -> model -> [input,cacheRead,cacheCreate,output,costNanos,n,n],
# verified 2026-05-18); the cache's `files` map is private project paths and
# is NEVER read. Cache filename schema churns (claude-v1/v2/...) — pick the
# highest version; on ANY structural mismatch exit non-zero so the caller
# publishes usage-only (fail-safe, never abort, never corrupt the payload).

# Add a 24h SESSION usage-% sparkline (`ph`) to Claude from CodexBar's hourly
# history file (~/Library/Application Support/com.steipete.codexbar/history/
# claude.json). This is usage %, NOT cost — feeds the Limits-card sparkline.
# Same fail-safe contract: any structural problem -> exit non-zero, caller
# publishes without `ph` (never abort, never corrupt the payload).

# Roll up Codex total spend / tokens / 30-day history from CodexBar's LOCAL
# cost cache and merge into the v2 payload. Primary: highest pi-sessions-v*.json
# (costNanos from HTTP cache — authoritative). Supplemental: highest codex-v*.json
# for days absent from pi-sessions (Cache.db eviction). pi-sessions schema:
# daysByProvider.codex[date][model].{costNanos,totalTokens}. codex-v schema:
# days[date][model]=[inp,cached,out] decoded via hardcoded pricing table. Same
# fail-safe contract as COST_MERGE_JXA: any structural mismatch -> exit non-zero
# so the caller publishes without Codex cost (never abort, never corrupt).

# Merge the reduced Pi Agent provider object emitted by pi-agent-stats.sh into
# the provider array. The helper output is sanitized here to the public payload
# contract so accidental extra local fields never cross the Upstash boundary.

# Merge the reduced LM Studio provider object emitted by lmstudio-stats.sh into
# the provider array. Same fail-safe contract as PI_MERGE_JXA: validates helper
# shape, extracts lm fields, and prepends/replaces. Any structural problem exits
# non-zero so the caller publishes without LM Studio (never abort, never corrupt).

# Patch `cu` onto the existing CodexBar `cursor` limits row (in-place, like
# COST_MERGE_JXA). Does NOT replace the provider (unlike LM_MERGE_JXA).

# Patch `oc` onto the existing CodexBar `opencodego` provider (in-place, like
# CURSOR_MERGE_JXA). Fail-soft: helper failure -> publish limits-only opencodego.

cmd_once() {
  mkdir -p "$LOG_DIR"
  [[ -x "$STATS" ]] || die "sibling codexbar-stats.sh not found/executable at $STATS"
  read_config
  local base="${MOCK_SINK_URL:-$UPSTASH_REST_URL}"
  [[ -n "$base" ]] || die "no UPSTASH_REST_URL (or MOCK_SINK_URL) in $CFG — see --help" 5

  # Single-flight: mkdir is atomic. Prevents an overlapping manual --once and
  # the scheduled launchd cycle from racing. NOT `local` (global EXIT trap).
  lockdir="$LOG_DIR/.publish.lock"
  if ! mkdir "$lockdir" 2>/dev/null; then
    log "skip: another publish cycle in progress ($lockdir) — keeping last good"; exit 0
  fi
  # NOTE: `work` is intentionally NOT `local` — the EXIT trap fires in global
  # scope after this function returns; a local would be unset there (set -u).
  work=""
  trap 'rm -rf "${work:-}"; rmdir "${lockdir:-/nonexistent/x}" 2>/dev/null' EXIT INT TERM
  work="$(mktemp -d "${TMPDIR:-/tmp}/cbpub.XXXXXX")" || die "mktemp failed"
  local json="$work/p.json" resp="$work/resp" kcfg="$work/curl.cfg"

  "$STATS" --json >"$json" 2>>"$LOG"; local rc=$?
  case $rc in
    0) : ;;  # >=1 provider returned real data — publish
    3) log "skip: codexbar-stats reported no fresh data (rc=3) — keeping last good"; exit 3 ;;
    *) log "skip: codexbar-stats failed (rc=$rc) — keeping last good"; exit 4 ;;
  esac
  local bytes; bytes=$(wc -c <"$json" | tr -d ' ')
  [[ "$bytes" -gt 2 ]] || { log "skip: empty stats payload — keeping last good"; exit 3; }

  # Augment Claude with total spend/tokens/30d history from the local cost
  # cache. Fail-safe: any cache problem -> publish the usage-only payload.
  if CBPUB_JSON="$json" osascript -l JavaScript "$SELF_DIR/lib/merge-cost.js" 2>>"$LOG"; then
    bytes=$(wc -c <"$json" | tr -d ' ')
  else
    log "note: Claude cost-cache merge skipped (absent/unrecognized) — publishing usage-only"
  fi

  # Augment Codex with total spend/tokens/30d history from pi-sessions cache.
  # Same fail-safe contract: any cache problem -> publish without Codex cost.
  if CBPUB_JSON="$json" osascript -l JavaScript "$SELF_DIR/lib/merge-codex-cost.js" 2>>"$LOG"; then
    bytes=$(wc -c <"$json" | tr -d ' ')
  else
    log "note: Codex cost-cache merge skipped (absent/unrecognized) — publishing usage-only"
  fi

  # Add the 24h SESSION usage-% sparkline (`ph`). Independent + fail-safe: a
  # missing/churned history file just omits `ph`, never blocks the publish.
  if CBPUB_JSON="$json" osascript -l JavaScript "$SELF_DIR/lib/merge-pct.js" 2>>"$LOG"; then
    bytes=$(wc -c <"$json" | tr -d ' ')
  else
    log "note: Claude 24h pct-history skipped (absent/unrecognized)"
  fi

  # Append/replace the Pi Agent provider from local ~/.pi/agent state. This is
  # deliberately independent of CodexBar's unrelated pi-sessions cost cache.
  # Timeout helper to prevent unbounded runtime during publish cycle.
  local pi_json="$work/pi.json"
  local pi_rc=127
  if [[ -x "$PI_STATS" ]]; then
    if command -v timeout >/dev/null 2>&1; then
      timeout 30 "$PI_STATS" >"$pi_json" 2>>"$LOG"; pi_rc=$?
    elif command -v gtimeout >/dev/null 2>&1; then
      gtimeout 30 "$PI_STATS" >"$pi_json" 2>>"$LOG"; pi_rc=$?
    else
      "$PI_STATS" >"$pi_json" 2>>"$LOG"; pi_rc=$?
    fi
    if [[ $pi_rc -eq 0 ]]; then
      if CBPUB_JSON="$json" CBPUB_PI_JSON="$pi_json" osascript -l JavaScript "$SELF_DIR/lib/merge-pi.js" 2>>"$LOG"; then
        bytes=$(wc -c <"$json" | tr -d ' ')
      else
        log "note: Pi Agent merge skipped (malformed helper output) — publishing without Pi"
      fi
    elif [[ $pi_rc -eq 124 ]]; then
      log "note: Pi Agent helper timed out after 30s — publishing without Pi"
    else
      log "note: Pi Agent helper failed (exit code $pi_rc) — publishing without Pi"
    fi
  else
    log "note: Pi Agent stats skipped (absent/unrecognized) — publishing without Pi"
  fi

  # Append/replace the LM Studio provider from local lmstudio-stats.sh.
  # Same fail-safe contract as Pi Agent: timeout guard, exit-code checking.
  local lm_json="$work/lm.json"
  local lm_rc=127
  if [[ -x "$LM_STATS" ]]; then
    if command -v timeout >/dev/null 2>&1; then
      timeout 30 "$LM_STATS" >"$lm_json" 2>>"$LOG"; lm_rc=$?
    elif command -v gtimeout >/dev/null 2>&1; then
      gtimeout 30 "$LM_STATS" >"$lm_json" 2>>"$LOG"; lm_rc=$?
    else
      "$LM_STATS" >"$lm_json" 2>>"$LOG"; lm_rc=$?
    fi
    if [[ $lm_rc -eq 0 ]]; then
      if CBPUB_JSON="$json" CBPUB_LM_JSON="$lm_json" osascript -l JavaScript "$SELF_DIR/lib/merge-lm.js" 2>>"$LOG"; then
        bytes=$(wc -c <"$json" | tr -d ' ')
      else
        log "note: LM Studio merge skipped (malformed helper output) — publishing without LM Studio"
      fi
    elif [[ $lm_rc -eq 124 ]]; then
      log "note: LM Studio helper timed out after 30s — publishing without LM Studio"
    else
      log "note: LM Studio helper failed (exit code $lm_rc) — publishing without LM Studio"
    fi
  else
    log "note: LM Studio stats skipped (absent/unrecognized) — publishing without LM Studio"
  fi

  # Patch `cu` onto the existing cursor limits row from cursor-stats.sh.
  # Fail-safe: helper failure -> publish limits-only cursor (never abort).
  local cursor_json="$work/cursor.json"
  local cursor_rc=127
  if [[ -x "$CURSOR_STATS" ]]; then
    if command -v timeout >/dev/null 2>&1; then
      timeout 30 "$CURSOR_STATS" >"$cursor_json" 2>>"$LOG"; cursor_rc=$?
    elif command -v gtimeout >/dev/null 2>&1; then
      gtimeout 30 "$CURSOR_STATS" >"$cursor_json" 2>>"$LOG"; cursor_rc=$?
    else
      "$CURSOR_STATS" >"$cursor_json" 2>>"$LOG"; cursor_rc=$?
    fi
    if [[ $cursor_rc -eq 0 ]]; then
      if CBPUB_JSON="$json" CBPUB_CURSOR_JSON="$cursor_json" osascript -l JavaScript "$SELF_DIR/lib/merge-cursor.js" 2>>"$LOG"; then
        bytes=$(wc -c <"$json" | tr -d ' ')
      else
        log "note: Cursor token merge skipped (malformed helper output) — publishing limits-only cursor"
      fi
    elif [[ $cursor_rc -eq 124 ]]; then
      log "note: Cursor stats helper timed out after 30s — publishing limits-only cursor"
    else
      log "note: Cursor stats helper failed (exit code $cursor_rc) — publishing limits-only cursor"
    fi
  else
    log "note: Cursor stats skipped (absent/unrecognized) — publishing limits-only cursor"
  fi

  # Patch `oc` onto the existing CodexBar `opencodego` provider from opencodego-stats.sh.
  # Fail-safe: helper failure -> publish limits-only opencodego (never abort).
  local og_json="$work/opencodego.json"
  local og_rc=127
  if [[ -x "$OPENCODE_GO_STATS" ]]; then
    if command -v timeout >/dev/null 2>&1; then
      timeout 30 "$OPENCODE_GO_STATS" >"$og_json" 2>>"$LOG"; og_rc=$?
    elif command -v gtimeout >/dev/null 2>&1; then
      gtimeout 30 "$OPENCODE_GO_STATS" >"$og_json" 2>>"$LOG"; og_rc=$?
    else
      "$OPENCODE_GO_STATS" >"$og_json" 2>>"$LOG"; og_rc=$?
    fi
    if [[ $og_rc -eq 0 ]]; then
      if CBPUB_JSON="$json" CBPUB_OG_JSON="$og_json" osascript -l JavaScript "$SELF_DIR/lib/merge-og.js" 2>>"$LOG"; then
        bytes=$(wc -c <"$json" | tr -d ' ')
      else
        log "note: OpenCode Go merge skipped (malformed helper output) — publishing limits-only opencodego"
      fi
    elif [[ $og_rc -eq 124 ]]; then
      log "note: OpenCode Go helper timed out after 30s — publishing limits-only opencodego"
    else
      log "note: OpenCode Go helper failed (exit code $og_rc) — publishing limits-only opencodego"
    fi
  else
    log "note: OpenCode Go stats skipped (absent/unrecognized) — publishing limits-only opencodego"
  fi

  local tok; tok="$(get_token)"
  [[ -n "$tok" ]] || die "no Upstash token in Keychain — run: codexbar-publish.sh --set-token" 5

  # Secret goes in a 0600 -K config, not argv (no token in `ps`).
  ( umask 077; print -r -- "header = \"Authorization: Bearer $tok\"" >"$kcfg" )
  local url="${base%/}/set/${UPSTASH_KEY}"
  local code
  code=$(curl -sS -K "$kcfg" -o "$resp" -w '%{http_code}' --max-time 15 \
           -X POST --data-binary @"$json" "$url" 2>>"$LOG") || true
  rm -f "$kcfg"

  if [[ "$code" == 2* ]]; then
    local dest="Upstash"; [[ -n "$MOCK_SINK_URL" ]] && dest="mock sink"
    log "published ${bytes}B to ${UPSTASH_KEY} (HTTP $code) via $dest"
    exit 0
  fi
  log "publish FAILED (HTTP ${code:-none}); response: $(head -c 200 "$resp" 2>/dev/null)"
  exit 4
}

render_plist() {
  [[ -r "$TPL" ]] || die "plist template missing at $TPL"
  local cb; cb="${CODEXBAR_BIN:-$(command -v codexbar 2>/dev/null)}"
  [[ -n "$cb" ]] || die "codexbar not found — install it or set CODEXBAR_BIN before --install"

  # The codex provider is fetched via `--source cli`, which execs the `codex`
  # CLI — a Node script that itself needs `node` on PATH. Version managers
  # (nvm/asdf/volta) install both in a per-version dir that is NEVER on
  # launchd's sparse PATH, so without this the codex fetch fails under launchd
  # (env: node: No such file or directory) and the toy shows codex as "off"
  # while the GUI shows real data. Resolve those dirs from the (interactive)
  # shell that runs --install — same best-effort `command -v` approach as
  # CODEXBAR_BIN, not a hardcoded Node version that would rot on upgrade.
  #
  # NOTE: this MUST run before `local path=` below. In zsh `path` is tied to
  # $PATH, so assigning it rewrites the command search path; resolving these
  # afterwards would search only the sparse list and find nothing.
  local base="/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin"
  local extra="" bin dir
  for bin in "${CODEX_BIN:-$(command -v codex 2>/dev/null)}" "$(command -v node 2>/dev/null)"; do
    [[ -n "$bin" && -x "$bin" ]] || continue
    dir="${bin:h}"
    case ":$extra:$base:" in (*":$dir:"*) ;; (*) extra="${extra:+$extra:}$dir" ;; esac
  done
  local path="${extra:+$extra:}$base"
  local t; t="$(<"$TPL")"
  t="${t//__SCRIPT__/$SELF_DIR/codexbar-publish.sh}"
  t="${t//__INTERVAL__/$PUBLISH_INTERVAL}"
  t="${t//__LOG__/$LOG}"
  t="${t//__CODEXBAR_BIN__/$cb}"
  t="${t//__PATH__/$path}"
  print -r -- "$t"
}

cmd_install() {
  read_config
  mkdir -p "$LOG_DIR" "$HOME/Library/LaunchAgents"
  render_plist > "$PLIST" || die "could not write $PLIST"
  plutil -lint "$PLIST" >/dev/null || die "generated plist failed plutil -lint"
  launchctl bootout "gui/$UID" "$PLIST" 2>/dev/null
  launchctl bootstrap "gui/$UID" "$PLIST" || die "launchctl bootstrap failed"
  launchctl enable "gui/$UID/$LABEL" 2>/dev/null
  launchctl kickstart -k "gui/$UID/$LABEL" 2>/dev/null
  log "installed launchd job $LABEL (interval ${PUBLISH_INTERVAL}s); log: $LOG"
}

cmd_uninstall() {
  launchctl bootout "gui/$UID" "$PLIST" 2>/dev/null
  rm -f "$PLIST"
  log "uninstalled launchd job $LABEL"
}

cmd_status() {
  read_config
  print -r -- "label:        $LABEL"
  print -r -- "plist:        $([[ -f $PLIST ]] && echo present || echo absent)"
  print -r -- "loaded:       $(launchctl print "gui/$UID/$LABEL" >/dev/null 2>&1 && echo yes || echo no)"
  print -r -- "config:       $([[ -r $CFG ]] && echo "$CFG" || echo 'missing (see --help)')"
  print -r -- "target:       ${MOCK_SINK_URL:-${UPSTASH_REST_URL:-<unset>}}/set/${UPSTASH_KEY}"
  print -r -- "token:        $([[ -n "$(get_token)" ]] && echo 'in Keychain' || echo 'MISSING — run --set-token')"
  print -r -- "cursor sess:  $([[ -n "$(get_cursor_session)" ]] && echo 'in Keychain' || echo 'MISSING — run --set-cursor-session')"
  print -r -- "cursor stats: $([[ -x "$CURSOR_STATS" ]] && echo "$CURSOR_STATS" || echo "MISSING/not executable — $CURSOR_STATS")"
  print -r -- "og sess:      $([[ -n "$(get_opencodego_session)" ]] && echo 'in Keychain' || echo 'MISSING — run --set-opencodego-cookie')"
  print -r -- "og stats:     $([[ -x "$OPENCODE_GO_STATS" ]] && echo "$OPENCODE_GO_STATS" || echo "MISSING/not executable — $OPENCODE_GO_STATS")"
  print -r -- "--- last log lines ---"
  [[ -f "$LOG" ]] && tail -n 8 "$LOG" || print -r -- "(no log yet)"
}

case "${1:---once}" in
  --once|"")   cmd_once ;;
  --set-token) cmd_set_token ;;
  --set-cursor-session) cmd_set_cursor_session ;;
  --set-cursor-session-clipboard) cmd_set_cursor_session_clipboard ;;
  --set-opencodego-cookie) cmd_set_opencodego_cookie ;;
  --set-opencodego-cookie-clipboard) cmd_set_opencodego_cookie_clipboard ;;
  --install)   cmd_install ;;
  --uninstall) cmd_uninstall ;;
  --status)    cmd_status ;;
  --print-plist) read_config; render_plist ;;
  -h|--help)   help ;;
  *)           print -r -- "unknown argument: $1 (try --help)" >&2; exit 2 ;;
esac
