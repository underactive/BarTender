#!/bin/zsh
# uninstall.sh — BarTender cleanup / uninstall helper
#
# Removes the LaunchAgent schedule, wipes stored credentials from the macOS
# Keychain, and deletes the publisher scripts from this repo.
#
#   ./scripts/uninstall.sh                         # full uninstall (interactive)
#   ./scripts/uninstall.sh --launchd-only          # only remove launchd schedule
#   ./scripts/uninstall.sh --keys-only             # only wipe Keychain tokens
#   ./scripts/uninstall.sh --scripts-only          # only delete scripts from disk
#   ./scripts/uninstall.sh --dry-run               # preview what would be done
#   ./scripts/uninstall.sh --help
#
# All destructive operations require explicit confirmation.
# The config file at ~/.config/codexbar-toy/config is preserved by default
# (set --full to delete it too).

set -euo pipefail

# ─── Colors ───────────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
DIM='\033[2m'
NC='\033[0m'

# ─── Globals ──────────────────────────────────────────────────────────────────
SELF_DIR="${0:A:h}"
KC_SERVICE="codexbar-toy"
KC_ACCOUNT="publish"
CURSOR_KC_ACCOUNT="cursor-session"
PLIST_FILE="$HOME/Library/LaunchAgents/com.codexbar-toy.publish.plist"
CFG_DIR="$HOME/.config/codexbar-toy"
CFG_FILE="$CFG_DIR/config"
LOG_DIR="$HOME/Library/Logs/codexbar-toy"

# ─── Helpers ──────────────────────────────────────────────────────────────────
info()  { printf "${CYAN}ℹ %s${NC}\n" "$*"; }
ok()    { printf "${GREEN}✓ %s${NC}\n" "$*"; }
warn()  { printf "${YELLOW}⚠ %s${NC}\n" "$*"; }
fail()  { printf "${RED}✗ %s${NC}\n" "$*"; }
hdr()   { printf "\n${BOLD}— %s —${NC}\n" "$*"; }

confirm() {
  printf "${BOLD}${YELLOW}%s${NC} [Y/n] " "$*"
  local resp
  read -r resp
  case "$resp" in
    [Yy]*|"") return 0 ;;
    [Nn]*) return 1 ;;
    *) return 1 ;;
  esac
}

# ─── Actions ──────────────────────────────────────────────────────────────────

remove_launchd() {
  hdr "Removing LaunchAgent"

  if [[ ! -f "$PLIST_FILE" ]]; then
    info "No LaunchAgent plist found at $PLIST_FILE — skipping"
    return 0
  fi

  local label
  label=$(plutil -extract Label raw "$PLIST_FILE" 2>/dev/null || echo "com.codexbar-toy.publish")

  info "Unloading $label ..."
  launchctl bootout "gui/$UID" "$PLIST_FILE" 2>/dev/null || true

  rm -f "$PLIST_FILE"
  ok "LaunchAgent removed ($PLIST_FILE)"
}

wipe_keychain_tokens() {
  hdr "Wiping Keychain Tokens"

  # Write token
  info "Removing Upstash write token (service=$KC_SERVICE, account=$KC_ACCOUNT) ..."
  security delete-generic-password -s "$KC_SERVICE" -a "$KC_ACCOUNT" 2>/dev/null
  if [[ $? -eq 0 || $? -eq 44 ]]; then
    # exit 44 = "item not found" — fine, means it was already gone
    ok "Upstash write token removed"
  else
    warn "Upstash write token — could not verify removal (may already be gone)"
  fi

  # Cursor session token (if any)
  info "Removing Cursor session token (service=$KC_SERVICE, account=$CURSOR_KC_ACCOUNT) ..."
  security delete-generic-password -s "$KC_SERVICE" -a "$CURSOR_KC_ACCOUNT" 2>/dev/null
  if [[ $? -eq 0 || $? -eq 44 ]]; then
    ok "Cursor session token removed"
  else
    warn "Cursor session token — could not verify removal (may already be gone)"
  fi
}

delete_scripts() {
  hdr "Deleting Scripts"

  local scripts=(
    "codexbar-publish.sh"
    "cursor-stats.sh"
    "lmstudio-stats.sh"
    "pi-agent-stats.sh"
  )

  for s in "${scripts[@]}"; do
    if [[ -f "$SELF_DIR/$s" ]]; then
      rm -f "$SELF_DIR/$s"
      ok "Deleted $s"
    else
      info "$s not found — skipping"
    fi
  done

  # Also remove __pycache__
  local pycache="$SELF_DIR/__pycache__"
  if [[ -d "$pycache" ]]; then
    rm -rf "$pycache"
    ok "Removed __pycache__"
  fi
}

remove_config() {
  hdr "Removing Config"

  if [[ -d "$CFG_DIR" ]]; then
    rm -rf "$CFG_DIR"
    ok "Removed config directory ($CFG_DIR)"
  else
    info "Config directory already gone — skipping"
  fi

  if [[ -d "$LOG_DIR" ]]; then
    rm -rf "$LOG_DIR"
    ok "Removed log directory ($LOG_DIR)"
  else
    info "Log directory already gone — skipping"
  fi
}

# ─── Dry Run ──────────────────────────────────────────────────────────────────

dry_run() {
  hdr "Dry Run — What Would Be Done"

  info "LaunchAgent:"
  if [[ -f "$PLIST_FILE" ]]; then
    info "  → unload + delete $PLIST_FILE"
  else
    info "  (not present)"
  fi

  info "Keychain tokens:"
  info "  → delete service=$KC_SERVICE account=$KC_ACCOUNT (Upstash write)"
  info "  → delete service=$KC_SERVICE account=$CURSOR_KC_ACCOUNT (Cursor session)"

  info "Scripts:"
  local s
  for s in codexbar-publish.sh cursor-stats.sh lmstudio-stats.sh pi-agent-stats.sh; do
    if [[ -f "$SELF_DIR/$s" ]]; then
      info "  → delete $SELF_DIR/$s"
    else
      info "  (not present: $s)"
    fi
  done

  info "Config: $CFG_DIR → remove directory"
  info "Logs:  $LOG_DIR → remove directory"
}

# ─── Main ─────────────────────────────────────────────────────────────────────

main() {
  local mode="${1:---full}"

  case "$mode" in
    --full)
      hdr "BarTender Uninstall"
      warn "This will remove the launchd schedule, wipe Keychain tokens,"
      warn "and delete publisher scripts from the repo."
      warn "The config file (~/.config/codexbar-toy/config) will also be removed."

      if confirm "Proceed with full uninstall?"; then
        remove_launchd
        wipe_keychain_tokens
        delete_scripts
        remove_config
        hdr "Done"
        ok "BarTender has been uninstalled."
      else
        info "Cancelled."
      fi
      ;;

    --launchd-only)
      info "Removing LaunchAgent only."
      if confirm "Proceed?"; then
        remove_launchd
      fi
      ;;

    --keys-only)
      info "Wiping Keychain tokens only."
      if confirm "Proceed?"; then
        wipe_keychain_tokens
      fi
      ;;

    --scripts-only)
      info "Deleting scripts only."
      if confirm "Proceed?"; then
        delete_scripts
      fi
      ;;

    --config-only)
      info "Removing config and logs only."
      if confirm "Proceed?"; then
        remove_config
      fi
      ;;

    --dry-run)
      dry_run
      ;;

    -h|--help)
      printf "Usage: %s [--full|--launchd-only|--keys-only|--scripts-only|--config-only|--dry-run|--help]\n\n" "$0"
      printf "  --full            Full uninstall (all of the above)\n"
      printf "  --launchd-only    Remove only the launchd schedule\n"
      printf "  --keys-only       Wipe only Keychain tokens\n"
      printf "  --scripts-only    Delete only scripts from disk\n"
      printf "  --config-only     Remove config (~/.config/codexbar-toy) and logs\n"
      printf "  --dry-run         Preview what would be done (no changes)\n"
      printf "  --help            Show this help\n\n"
      printf "All destructive operations require confirmation.\n"
      ;;

    *)
      fail "Unknown option: $mode"
      printf "Try --help for usage.\n"
      exit 2
      ;;
  esac
}

main "$@"
