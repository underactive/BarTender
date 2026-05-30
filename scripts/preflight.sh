#!/bin/zsh
# preflight.sh — BarTender setup assistant
#
# Checks all dependencies (CodexBar, ESP-IDF, Python deps), guides through
# missing pieces, and optionally installs launchd + flashes the device.
#
#   ./scripts/preflight.sh                 # check + guide mode
#   ./scripts/preflight.sh --install       # check + guide + auto-install
#   ./scripts/preflight.sh --flash         # check deps + flash firmware
#   ./scripts/preflight.sh --check         # read-only check (alias for no args)
#
# Destructive operations (--install, --flash) require explicit confirmation.
# Always prompts before writing, deleting, or flashing.

set -euo pipefail

# ─── Colors ───────────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
DIM='\033[2m'
NC='\033[0m'  # No Color

# ─── Globals ──────────────────────────────────────────────────────────────────
SELF_DIR="${0:A:h}"
SELF_NAME="${0:t}"
REPO_ROOT="${SELF_DIR:A:h}"
CFG_DIR="$HOME/.config/codexbar-toy"
CFG_FILE="$CFG_DIR/config"
LOG_DIR="$HOME/Library/Logs/codexbar-toy"
PLIST_FILE="$HOME/Library/LaunchAgents/com.codexbar-toy.publish.plist"
KC_SERVICE="codexbar-toy"
KC_ACCOUNT="publish"
CURSOR_KC_ACCOUNT="cursor-session"

# Upstash defaults
UPSTASH_REST_URL=""
UPSTASH_KEY="codexbar"
PUBLISH_INTERVAL="300"

# ─── Helpers ──────────────────────────────────────────────────────────────────
info()  { printf "${CYAN}ℹ %s${NC}\n" "$*"; }
ok()    { printf "${GREEN}✓ %s${NC}\n" "$*"; }
warn()  { printf "${YELLOW}⚠ %s${NC}\n" "$*"; }
fail()  { printf "${RED}✗ %s${NC}\n" "$*"; }
hdr()   { printf "\n${BOLD}— %s —${NC}\n" "$*"; }
sub()   { printf "\n${DIM}  $*${NC}\n"; }

confirm() {
  # Asks user for confirmation. Returns 0 if yes, 1 if no.
  printf "${BOLD}${YELLOW}%s${NC} [Y/n] " "$*"
  local resp
  read -r resp
  case "$resp" in
    [Yy]*|"") return 0 ;;
    [Nn]*) return 1 ;;
    *) return 1 ;;
  esac
}

read_config() {
  # Parse non-secret config from ~/.config/codexbar-toy/config
  [[ -r "$CFG_FILE" ]] || return 0
  local line k v
  while IFS= read -r line || [[ -n "$line" ]]; do
    line="${line%%#*}"
    [[ "$line" == *=* ]] || continue
    k="${${line%%=*}// /}"
    v="${line#*=}"
    v="${v## }"
    v="${v%% }"
    v="${v#[\"\']}"
    v="${v%[\"\']}"
    case "$k" in
      UPSTASH_REST_URL) UPSTASH_REST_URL="$v" ;;
      UPSTASH_KEY)      UPSTASH_KEY="${v:-codexbar}" ;;
      PUBLISH_INTERVAL) PUBLISH_INTERVAL="${v:-300}" ;;
    esac
  done < "$CFG_FILE"
}

check_exists() {
  # Returns 0 if command/path exists, 1 otherwise
  command -v "$1" &>/dev/null || [[ -e "$1" ]]
}

get_keychain_token() {
  security find-generic-password -s "$KC_SERVICE" -a "$KC_ACCOUNT" -w 2>/dev/null
}

has_keychain_token() {
  get_keychain_token &>/dev/null
}

# ─── Dependency Checks ────────────────────────────────────────────────────────
declare -A dep_status  # dep_name -> "ok" | "warn" | "missing"

check_codexbar() {
  local bin
  bin="${CODEXBAR_BIN:-$(command -v codexbar 2>/dev/null)}"
  if [[ -n "$bin" && -x "$bin" ]]; then
    local ver
    ver=$("$bin" --version 2>/dev/null || echo "unknown")
    ok "CodexBar CLI found at $bin ($ver)"
    dep_status[codexbar]="ok"
    return 0
  elif [[ -n "$CODEXBAR_BIN" ]]; then
    warn "CODEXBAR_BIN=$CODEXBAR_BIN exists but is not executable"
    dep_status[codexbar]="warn"
    return 1
  else
    fail "CodexBar CLI not found on PATH"
    warn "  Install from https://github.com/steipete/CodexBar"
    warn "  or set CODEXBAR_BIN to the binary path"
    dep_status[codexbar]="missing"
    return 1
  fi
}

check_codexbar_config() {
  if [[ -r "$HOME/.codexbar/config.json" ]]; then
    local providers
    providers=$(python3 -c "import json; d=json.load(open('$HOME/.codexbar/config.json')); print(len(d.get('providers',[])))" 2>/dev/null || echo "?")
    ok "CodexBar config found ($providers providers configured)"
    return 0
  else
    warn "No CodexBar config at ~/.codexbar/config.json"
    warn "  The CodexBar app must be installed and have at least one provider enabled"
    return 1
  fi
}

check_esp_idf() {
  if command -v idf.py &>/dev/null; then
    local ver
    ver=$(idf.py --version 2>/dev/null | head -1 || echo "unknown")
    ok "ESP-IDF found ($ver)"
    dep_status[esp-idf]="ok"
    return 0
  else
    fail "ESP-IDF not found — idf.py not on PATH"
    warn "  Install: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/"
    warn "  Or set PATH to include the ESP-IDF tools directory"
    dep_status[esp-idf]="missing"
    return 1
  fi
}

check_python_deps() {
  local missing=()

  if ! command -v python3 &>/dev/null; then
    fail "Python 3 not found"
    warn "  brew install python3"
    missing+=(python3)
    dep_status[python]="missing"
    return 1
  fi

  local pyver
  pyver=$(python3 --version 2>/dev/null)
  ok "Python 3 found ($pyver)"
  dep_status[python]="ok"

  # Pillow — needed for icon generation
  if ! python3 -c "import PIL" &>/dev/null; then
    warn "Pillow (Python) not installed"
    warn "  pip3 install Pillow  (needed for scripts/gen-provider-icons.py)"
    missing+=(pillow)
    dep_status[pillow]="missing"
  else
    ok "Pillow installed"
    dep_status[pillow]="ok"
  fi

  # pyserial — needed for screenshot protocol
  if ! python3 -c "import serial" &>/dev/null; then
    warn "pyserial not installed"
    warn "  pip3 install pyserial  (needed for scripts/screenshot.py)"
    missing+=(pyserial)
    dep_status[pyserial]="missing"
  else
    ok "pyserial installed"
    dep_status[pyserial]="ok"
  fi

  # rsvg-convert — needed for SVG→PNG icon generation
  if ! command -v rsvg-convert &>/dev/null; then
    warn "rsvg-convert not found"
    warn "  brew install librsvg  (needed for scripts/gen-provider-icons.py)"
    missing+=(rsvg-convert)
    dep_status[rsvg-convert]="missing"
  else
    ok "rsvg-convert installed"
    dep_status[rsvg-convert]="ok"
  fi

  if (( ${#missing[@]} > 0 )); then
    warn "Missing Python deps: ${missing[*]}"
    return 1
  fi
  return 0
}

check_upstash_config() {
  read_config

  if [[ -z "$UPSTASH_REST_URL" ]]; then
    fail "No Upstash REST URL in $CFG_FILE"
    warn "  Create a free DB at https://console.upstash.com"
    warn "  Then run: $CFG_DIR/config with UPSTASH_REST_URL and UPSTASH_KEY"
    dep_status[upstash-config]="missing"
    return 1
  fi

  ok "Upstash config found (${UPSTASH_REST_URL%/*}...)"
  dep_status[upstash-config]="ok"

  if ! has_keychain_token; then
    warn "No Upstash write token in Keychain"
    warn "  Run: codexbar-publish.sh --set-token"
    dep_status[upstash-token]="missing"
    return 1
  fi
  ok "Upstash write token found in Keychain (service=$KC_SERVICE)"
  dep_status[upstash-token]="ok"

  return 0
}

check_launchd() {
  if [[ -f "$PLIST_FILE" ]]; then
    local label
    label=$(plutil -extract Label raw "$PLIST_FILE" 2>/dev/null || echo "unknown")
    local state
    state=$(launchctl list "$label" 2>/dev/null | head -1 || echo "not loaded")
    if echo "$state" | grep -q "com.codexbar-toy.publish"; then
      ok "LaunchAgent installed and loaded ($PLIST_FILE)"
      dep_status[launchd]="ok"
      return 0
    else
      ok "LaunchAgent plist exists but not currently loaded"
      warn "  It will run at next login. Use --install to activate now."
      dep_status[launchd]="warn"
      return 1
    fi
  else
    warn "No LaunchAgent plist at $PLIST_FILE"
    warn "  Run --install to create and start the schedule"
    dep_status[launchd]="missing"
    return 1
  fi
}

check_hardware() {
  # Check for ESP32-S3 connected via USB serial port
  local ports=()
  local found=0

  # ESP32 native USB-CDC: /dev/tty.usbmodem*
  while IFS= read -r port; do
    ports+=("$port")
    # Try to identify the chip
    local chip_info
    chip_info=$(system_profiler SPUSBDataType 2>/dev/null | grep -B5 -A1 "usbmodem" | head -10 || echo "unknown")
    ok "ESP32 serial port found: $port"
    ok "  $chip_info"
    found=1
  done < <(ls /dev/tty.usbmodem* 2>/dev/null || true)

  if (( ! found )); then
    fail "No ESP32-S3 detected on USB"
    warn "  Connect the Freenove FNK0104 via USB-C and try again"
    warn "  The board may use a CH343 or CP2102 USB-serial bridge"
    warn "  If it was recently flashed, wait ~30s for the driver to settle"
    dep_status[hardware]="missing"
    return 1
  fi

  ok "Hardware detected (${#ports[@]} port(s))"
  dep_status[hardware]="ok"
  return 0
}

# ─── Interactive Setup Helpers ────────────────────────────────────────────────

setup_upstash_config() {
  hdr "Setting up Upstash"

  if [[ ! -d "$CFG_DIR" ]]; then
    mkdir -p "$CFG_DIR"
    chmod 700 "$CFG_DIR"
    ok "Created config directory: $CFG_DIR"
  fi

  if [[ -z "$UPSTASH_REST_URL" ]]; then
    local url key
    read -r -p "  Upstash REST URL (e.g. https://xxx.upstash.io): " url
    UPSTASH_REST_URL="$url"

    read -r -p "  Redis key (default: codexbar): " key
    UPSTASH_KEY="${key:-codexbar}"

    read -r -p "  Publish interval in seconds (default: 300 / 5 min): " interval
    PUBLISH_INTERVAL="${interval:-300}"

    # Write config safely (KEY=VALUE, no eval)
    cat > "$CFG_FILE" <<EOF
# BarTender publisher config (see docs/ONBOARDING.md)
UPSTASH_REST_URL=$UPSTASH_REST_URL
UPSTASH_KEY=$UPSTASH_KEY
PUBLISH_INTERVAL=$PUBLISH_INTERVAL
EOF
    chmod 600 "$CFG_FILE"
    ok "Config written to $CFG_FILE"
  else
    ok "Upstash config already present"
  fi

  # Write token if missing
  if ! has_keychain_token; then
    warn "No Upstash write token in Keychain"
    if confirm "Set the write token now?"; then
      # Delegate to the existing publisher's --set-token (handles Keychain securely)
      "$SELF_DIR/codexbar-publish.sh" --set-token
      ok "Write token stored in Keychain"
    fi
  fi
}

# ─── Install LaunchAgent ──────────────────────────────────────────────────────

install_launchd() {
  hdr "Installing LaunchAgent"

  [[ -r "$CFG_FILE" ]] || { warn "No config at $CFG_FILE — set up Upstash first"; return 1; }
  read_config

  # Ensure codexbar is available
  local cb
  cb=$(command -v codexbar 2>/dev/null)
  [[ -n "$cb" && -x "$cb" ]] || { warn "CodexBar not on PATH — launchd may fail to run"; }

  # Find a good PATH
  local plist_path
  # Try Homebrew first, then system paths
  local brew_paths=()
  while IFS= read -r bp; do
    [[ -d "$bp" ]] && brew_paths+=("$bp")
  done < <(echo "$PATH" | tr ':' '\n' | grep -E "(homebrew|Cellar|opt)" || true)

  if (( ${#brew_paths[@]} > 0 )); then
    plist_path="$(IFS=:; echo "${brew_paths[*]}"):/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
  else
    plist_path="/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
  fi

  # Ensure log directory
  mkdir -p "$LOG_DIR"
  chmod 700 "$LOG_DIR"

  # Render plist from template
  local tpl="$SELF_DIR/../launchd/com.codexbar-toy.publish.plist.template"
  if [[ ! -r "$tpl" ]]; then
    fail "LaunchAgent template not found at $tpl"
    return 1
  fi

  local content
  content="$(cat "$tpl")"
  content="${content//__SCRIPT__/$SELF_DIR/codexbar-publish.sh}"
  content="${content//__INTERVAL__/$PUBLISH_INTERVAL}"
  content="${content//__LOG__/$LOG_DIR/publish.log}"
  content="${content//__CODEXBAR_BIN__/$cb}"
  content="${content//__PATH__/$plist_path}"

  mkdir -p "$HOME/Library/LaunchAgents"
  echo "$content" > "$PLIST_FILE"
  chmod 644 "$PLIST_FILE"

  # Stop any existing job
  launchctl bootout "gui/$UID" "$PLIST_FILE" 2>/dev/null || true

  # Install and start
  launchctl bootstrap "gui/$UID" "$PLIST_FILE" || { fail "launchctl bootstrap failed"; return 1; }
  launchctl enable "gui/$UID/com.codexbar-toy.publish" 2>/dev/null || true
  launchctl kickstart -k "gui/$UID/com.codexbar-toy.publish" 2>/dev/null || true

  ok "LaunchAgent installed and started"
  ok "  Label: com.codexbar-toy.publish"
  ok "  Interval: ${PUBLISH_INTERVAL}s"
  ok "  Config: $CFG_FILE"
  ok "  Log: $LOG_DIR/publish.log"
}

# ─── Flash Firmware ───────────────────────────────────────────────────────────

flash_firmware() {
  hdr "Flashing firmware to ESP32-S3"

  # Check ESP-IDF
  if ! command -v idf.py &>/dev/null; then
    fail "ESP-IDF not found on PATH"
    fail "  Install: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/"
    return 1
  fi

  # Find the device
  local port
  port=$(ls /dev/tty.usbmodem* 2>/dev/null | head -1)
  if [[ -z "$port" ]]; then
    fail "No ESP32-S3 detected on USB"
    fail "  Connect the Freenove FNK0104 and retry"
    return 1
  fi

  # Check firmware exists
  local fw_dir="$REPO_ROOT/firmware"
  if [[ ! -d "$fw_dir" ]]; then
    fail "firmware/ directory not found at $fw_dir"
    return 1
  fi

  # Build
  local build_dir="$fw_dir/build"
  local has_build=0
  if [[ -d "$build_dir" && -d "$build_dir/bootloader" ]]; then
    has_build=1
  fi

  if (( ! has_build )); then
    warn "No pre-built firmware found — building first..."
    warn "  This may take a few minutes on first build (fetches managed deps)"
    if ! confirm "Build firmware now?"; then
      warn "Skipping build"
      return 1
    fi

    cd "$fw_dir"
    idf.py set-target esp32s3 2>&1 | tail -1
    idf.py build 2>&1 | tail -1
    cd "$REPO_ROOT"
    ok "Build complete"
  fi

  info "Flashing to $port ..."
  if ! confirm "Confirm flashing to $port?"; then
    warn "Flashing cancelled"
    return 1
  fi

  if idf.py -p "$port" flash 2>&1 | tail -5; then
    ok "Flash complete"
    info "To open serial monitor: idf.py -p $port monitor"
  else
    fail "Flash failed"
    warn "Check the USB connection and try again"
    return 1
  fi
}

# ─── Preflight Report ─────────────────────────────────────────────────────────

preflight_report() {
  hdr "Preflight Report"

  local missing=()
  local warnings=()

  for key in codexbar esp-idf python upstash-config hardware; do
    local val="${dep_status[$key]:-not checked}"
    case "$val" in
      ok)
        case "$key" in
          codexbar)    ok "CodexBar CLI" ;;
          esp-idf)     ok "ESP-IDF" ;;
          python)      ok "Python 3" ;;
          upstash-config) ok "Upstash config" ;;
          hardware)    ok "ESP32-S3 hardware" ;;
        esac
        ;;
      warn)
        case "$key" in
          codexbar)    warn "CodexBar CLI (not found on PATH)" ;;
          esp-idf)     warn "ESP-IDF (not on PATH)" ;;
          upstash-config) warn "Upstash config incomplete" ;;
          hardware)    warn "Hardware not connected" ;;
        esac
        warnings+=("$key")
        ;;
      missing)
        case "$key" in
          codexbar)    fail "CodexBar CLI (MISSING)" ;;
          esp-idf)     fail "ESP-IDF (MISSING)" ;;
          python)      fail "Python 3 (MISSING)" ;;
          upstash-config) fail "Upstash config (MISSING)" ;;
          hardware)    fail "ESP32-S3 hardware (NOT CONNECTED)" ;;
        esac
        missing+=("$key")
        ;;
    esac
  done

  # Python sub-deps
  for key in pillow pyserial rsvg-convert; do
    local val="${dep_status[$key]:-not checked}"
    case "$val" in
      ok)     ok "Python: $key" ;;
      missing) warn "Python: $key (optional)" ;;
    esac
  done

  # LaunchAgent
  if [[ -f "$PLIST_FILE" ]]; then
    ok "LaunchAgent"
  else
    warn "LaunchAgent (not installed)"
  fi

  # Summary
  local total_missing=${#missing[@]}
  local total_warnings=${#warnings[@]}

  if (( total_missing == 0 && total_warnings == 0 )); then
    printf "\n${GREEN}${BOLD}All checks passed — system ready!${NC}\n"
    return 0
  else
    printf "\n"
    if (( total_missing > 0 )); then
      printf "${RED}${BOLD}$total_missing check(s) failed — setup required${NC}\n"
    fi
    if (( total_warnings > 0 )); then
      printf "${YELLOW}${BOLD}$total_warnings warning(s)${NC}\n"
    fi
    return 1
  fi
}

# ─── Guide Mode ───────────────────────────────────────────────────────────────

guide_setup() {
  hdr "Setup Guide"
  local steps=()

  # 1. CodexBar
  if [[ "${dep_status[codexbar]:-missing}" == "missing" ]]; then
    warn "Step 1: Install CodexBar"
    warn "  → https://github.com/steipete/CodexBar"
    warn "  → Enable at least one AI provider in ~/.codexbar/config.json"
    steps+=(codexbar)
  fi

  # 2. ESP-IDF
  if [[ "${dep_status[esp-idf]:-missing}" == "missing" ]]; then
    warn "Step 2: Install ESP-IDF"
    warn "  → https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/"
    warn "  → Ensure idf.py is on PATH"
    steps+=(esp-idf)
  fi

  # 3. Python deps
  local python_missing=()
  if [[ "${dep_status[pillow]:-ok}" == "missing" ]]; then python_missing+=(Pillow); fi
  if [[ "${dep_status[pyserial]:-ok}" == "missing" ]]; then python_missing+=(pyserial); fi
  if [[ "${dep_status[rsvg-convert]:-ok}" == "missing" ]]; then python_missing+=(rsvg-convert); fi

  if (( ${#python_missing[@]} > 0 )); then
    warn "Step 3: Install Python dependencies"
    warn "  pip3 install ${python_missing[*]}"
    if [[ "${dep_status[rsvg-convert]:-ok}" == "missing" ]]; then
      warn "  brew install librsvg"
    fi
    steps+=("python-${python_missing[*]}")
  fi

  # 4. Upstash config
  if [[ "${dep_status[upstash-config]:-missing}" == "missing" ]]; then
    warn "Step 4: Set up Upstash"
    warn "  → Create DB at https://console.upstash.com"
    warn "  → Grab REST URL + write token + read-only token"
    warn "  → mkdir -p $CFG_DIR && chmod 700 $CFG_DIR"
    warn "  → Write config (see docs/ONBOARDING.md for format)"
    steps+=(upstash)
  fi

  # 5. Upstash token
  if [[ "${dep_status[upstash-token]:-missing}" == "missing" ]]; then
    warn "Step 5: Store Upstash write token in Keychain"
    warn "  Run: codexbar-publish.sh --set-token"
    steps+=(upstash-token)
  fi

  # 6. Connect hardware
  if [[ "${dep_status[hardware]:-missing}" == "missing" ]]; then
    warn "Step 6: Connect your Freenove FNK0104 via USB-C"
    steps+=(hardware)
  fi

  # 7. Install launchd (optional)
  warn "Step 7: Install launchd schedule (optional)"
  warn "  Run: ./$SELF_NAME --install"

  # 8. Flash (optional)
  warn "Step 8: Flash firmware (optional)"
  warn "  Run: ./$SELF_NAME --flash"

  if (( ${#steps[@]} == 0 )); then
    info "Everything is set up — no steps needed!"
  fi
}

# ─── Main ─────────────────────────────────────────────────────────────────────

main() {
  local mode="${1:---check}"
  local do_install=0
  local do_flash=0

  # Parse flags
  case "$mode" in
    --install) do_install=1 ;;
    --flash)   do_flash=1 ;;
    --check|"") ;;  # default mode
    -h|--help)
      awk 'NR>=2 && /^#/{sub(/^# ?/,"");print;next} NR>=2{exit}' "$0"
      exit 0
      ;;
    *) echo "Unknown option: $mode (try --check, --install, --flash, --help)"; exit 2 ;;
  esac

  hdr "BarTender Preflight"
  info "Running dependency checks..."
  sub "This reports status and optionally guides setup."
  sub "Destructive operations (--install, --flash) always require confirmation."

  # Run all checks
  check_codexbar 2>/dev/null || true
  check_codexbar_config 2>/dev/null || true
  check_esp_idf 2>/dev/null || true
  check_python_deps 2>/dev/null || true
  check_upstash_config 2>/dev/null || true
  check_launchd 2>/dev/null || true
  check_hardware 2>/dev/null || true

  # Report
  local failed=0
  preflight_report || failed=1

  # Guide if needed
  if (( failed )); then
    guide_setup
  fi

  # Auto-install if requested
  if (( do_install )); then
    if (( failed )); then
      warn "There are failing checks. You may want to fix them before --install."
    fi
    if confirm "Install launchd schedule and proceed?"; then
      # Check if we can proceed
      if [[ "${dep_status[upstash-config]:-missing}" == "missing" ]]; then
        warn "Upstash config is missing. Setting it up interactively..."
        setup_upstash_config
      fi

      if [[ "${dep_status[hardware]:-missing}" == "missing" ]]; then
        warn "No hardware detected. Skipping flash step."
      else
        flash_firmware || warn "Flash was attempted but may have failed."
      fi

      install_launchd || warn "LaunchAgent installation failed."
    fi
  elif (( do_flash )); then
    if (( failed )); then
      warn "There are failing checks. You may want to fix them before --flash."
    fi
    if confirm "Flash the firmware?"; then
      flash_firmware
    fi
  fi

  return 0
}

main "$@"
