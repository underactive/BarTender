#!/bin/zsh
# codexbar-publish.sh — POC (Prompt 2 of 3): publish a minimal, non-sensitive
# CodexBar usage JSON to Upstash Redis (REST) on a schedule, so the future
# ESP32 toy (Prompt 3) can read it with a read-only token.
#
#   codexbar-publish.sh                 # one publish cycle (same as --once)
#   codexbar-publish.sh --once
#   codexbar-publish.sh --set-token     # store the Upstash WRITE token (Keychain)
#   codexbar-publish.sh --install       # install + start the launchd schedule
#   codexbar-publish.sh --uninstall     # stop + remove the launchd schedule
#   codexbar-publish.sh --status        # job state + recent log + readiness
#   codexbar-publish.sh --print-plist   # render the launchd plist to stdout
#   codexbar-publish.sh --help
#
# Security model (see docs/SECURITY.md):
#   - Only the whitelisted `codexbar-stats.sh --json` payload leaves the Mac
#     (usage % + reset hints; NEVER emails/identity/credentials/$).
#   - Write token lives in the macOS Keychain, never in argv/log/plist.
#   - curl auth header is passed via a 0600 temp -K config, not the command
#     line (no secret in `ps`).
#   - If there is no fresh data, the publish is SKIPPED (the store keeps its
#     last good value — a transient local failure must not blank the toy).
#
# Non-secret config: ~/.config/codexbar-toy/config  (KEY=VALUE lines)
#   UPSTASH_REST_URL=https://<db>.upstash.io
#   UPSTASH_KEY=codexbar            # Redis key (default: codexbar)
#   PUBLISH_INTERVAL=300            # launchd seconds (default: 300)
#   MOCK_SINK_URL=                  # test override; if set, used instead of Upstash
#
# Zero third-party deps: codexbar-stats.sh + base-macOS security/curl/launchctl/
# awk/date/mktemp + zsh builtins.
set -u

LABEL="com.codexbar-toy.publish"
# Test overrides (hermetic verification; default to real user paths):
KC_SERVICE="${CBPUB_KC_SERVICE:-codexbar-toy}"; KC_ACCOUNT="publish"
CFG="${CBPUB_CONFIG:-$HOME/.config/codexbar-toy/config}"; CFG_DIR="${CFG:h}"
LOG_DIR="${CBPUB_LOG_DIR:-$HOME/Library/Logs/codexbar-toy}"; LOG="$LOG_DIR/publish.log"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
SELF_DIR="${0:A:h}"
STATS="$SELF_DIR/codexbar-stats.sh"
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

cmd_once() {
  mkdir -p "$LOG_DIR"
  [[ -x "$STATS" ]] || die "sibling codexbar-stats.sh not found/executable at $STATS"
  read_config
  local base="${MOCK_SINK_URL:-$UPSTASH_REST_URL}"
  [[ -n "$base" ]] || die "no UPSTASH_REST_URL (or MOCK_SINK_URL) in $CFG — see --help" 5

  # NOTE: `work` is intentionally NOT `local` — the EXIT trap fires in global
  # scope after this function returns; a local would be unset there (set -u).
  work="$(mktemp -d "${TMPDIR:-/tmp}/cbpub.XXXXXX")" || die "mktemp failed"
  local json="$work/p.json" resp="$work/resp" kcfg="$work/curl.cfg"
  trap 'rm -rf "${work:-}"' EXIT INT TERM

  "$STATS" --json >"$json" 2>>"$LOG"; local rc=$?
  case $rc in
    0) : ;;  # >=1 provider returned real data — publish
    3) log "skip: codexbar-stats reported no fresh data (rc=3) — keeping last good"; exit 3 ;;
    *) log "skip: codexbar-stats failed (rc=$rc) — keeping last good"; exit 4 ;;
  esac
  local bytes; bytes=$(wc -c <"$json" | tr -d ' ')
  [[ "$bytes" -gt 2 ]] || { log "skip: empty stats payload — keeping last good"; exit 3; }

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
  local path="/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin"
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
  print -r -- "--- last log lines ---"
  [[ -f "$LOG" ]] && tail -n 8 "$LOG" || print -r -- "(no log yet)"
}

case "${1:---once}" in
  --once|"")   cmd_once ;;
  --set-token) cmd_set_token ;;
  --install)   cmd_install ;;
  --uninstall) cmd_uninstall ;;
  --status)    cmd_status ;;
  --print-plist) read_config; render_plist ;;
  -h|--help)   help ;;
  *)           print -r -- "unknown argument: $1 (try --help)" >&2; exit 2 ;;
esac
