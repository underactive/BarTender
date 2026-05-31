#!/bin/zsh
# boot-capture.sh — Boot-sequence timelapse over USB serial
#
# Optionally resets the board, grabs N screenshots via scripts/build/screenshot.py,
# and assembles bootup.mp4 with ffmpeg. Each frame is a full 320×240 SCAP
# transfer (~1–3+ s), so playback speed won't match real-time boot. Backlight
# fade during boot is not visible in framebuffer captures — record the physical
# panel for that.
#
#   ./scripts/build/boot-capture.sh
#   ./scripts/build/boot-capture.sh --port /dev/cu.usbmodem14101 --frames 30
#   ./scripts/build/boot-capture.sh --no-reset --interval 1 --fps 4
#   ./scripts/build/boot-capture.sh --no-video
#   ./scripts/build/boot-capture.sh --help
#
# Requirements:
#   pip3 install pyserial Pillow   (screenshot.py)
#   brew install ffmpeg            (video assembly; skip with --no-video)
#   esptool for --reset (ESP-IDF or: pip3 install esptool)
#
# Close idf.py monitor on the serial port before running.
set -euo pipefail

err()  { print -r -- "error: $*" >&2; }
warn() { print -r -- "warning: $*" >&2; }
log()  { print -r -- "$*"; }

SELF_DIR="${0:A:h}"
REPO_ROOT="${SELF_DIR:A:h:h}"
SCREENSHOT="$SELF_DIR/screenshot.py"

BC_PORT=""
BC_OUT=""
BC_FRAMES=20
BC_INTERVAL=0.5
BC_DELAY=3
BC_RESET=1
BC_FPS=2
BC_VIDEO=1

while (( $# > 0 )); do
  case "$1" in
    --port)
      shift; BC_PORT="${1:-}"; [[ -n "$BC_PORT" ]] || { err "--port requires a value"; exit 2; }
      ;;
    --out)
      shift; BC_OUT="${1:-}"; [[ -n "$BC_OUT" ]] || { err "--out requires a value"; exit 2; }
      ;;
    --frames)
      shift; BC_FRAMES="${1:-}"; [[ "$BC_FRAMES" == <-> ]] || { err "--frames requires a number"; exit 2; }
      ;;
    --interval)
      shift; BC_INTERVAL="${1:-}"; [[ "$BC_INTERVAL" =~ ^[0-9]+(\.[0-9]+)?$ ]] || { err "--interval requires a number"; exit 2; }
      ;;
    --delay)
      shift; BC_DELAY="${1:-}"; [[ "$BC_DELAY" =~ ^[0-9]+(\.[0-9]+)?$ ]] || { err "--delay requires a number"; exit 2; }
      ;;
    --fps)
      shift; BC_FPS="${1:-}"; [[ "$BC_FPS" == <-> ]] || { err "--fps requires a number"; exit 2; }
      ;;
    --port=*)     BC_PORT="${1#*=}" ;;
    --out=*)      BC_OUT="${1#*=}" ;;
    --frames=*)   BC_FRAMES="${1#*=}" ;;
    --interval=*) BC_INTERVAL="${1#*=}" ;;
    --delay=*)    BC_DELAY="${1#*=}" ;;
    --fps=*)      BC_FPS="${1#*=}" ;;
    --reset)      BC_RESET=1 ;;
    --no-reset)   BC_RESET=0 ;;
    --no-video)   BC_VIDEO=0 ;;
    -h|--help)
      awk 'NR>=2 && /^#/{sub(/^# ?/,"");print;next} NR>=2{exit}' "$0"
      exit 0
      ;;
    *)
      err "unknown argument: $1 (try --help)"
      exit 2
      ;;
  esac
  shift
done

find_port() {
  python3 -c '
import sys
try:
    import serial.tools.list_ports
except ImportError:
    sys.exit(1)
candidates = [p.device for p in serial.tools.list_ports.comports()
              if "usbmodem" in p.device or "usbserial" in p.device]
cu = [p for p in candidates if "/cu." in p]
port = cu[0] if cu else (candidates[0] if candidates else "")
if port:
    print(port)
' 2>/dev/null
}

check_deps() {
  [[ -x "$SCREENSHOT" ]] || { err "missing $SCREENSHOT"; exit 1; }
  python3 -c 'import serial' 2>/dev/null || {
    err "pyserial not installed — run: pip3 install pyserial"
    exit 1
  }
  python3 -c 'from PIL import Image' 2>/dev/null || {
    err "Pillow not installed — run: pip3 install Pillow"
    exit 1
  }
  if (( BC_VIDEO )); then
    command -v ffmpeg >/dev/null 2>&1 || {
      err "ffmpeg not found — install ffmpeg or pass --no-video"
      exit 1
    }
  fi
}

reset_device() {
  local port="$1"
  if command -v esptool.py >/dev/null 2>&1; then
    esptool.py --chip esp32s3 -p "$port" run
    return
  fi
  if python3 -m esptool --help >/dev/null 2>&1; then
    python3 -m esptool --chip esp32s3 -p "$port" run
    return
  fi
  err "esptool not found (needed for --reset)."
  err "Install ESP-IDF, pip3 install esptool, or use --no-reset and press EN/RST."
  exit 1
}

check_deps

port="$BC_PORT"
if [[ -z "$port" ]]; then
  port="$(find_port)" || true
  if [[ -z "$port" ]]; then
    err "no USB serial port found."
    err "Connect the board or pass: --port /dev/cu.usbmodem14101"
    exit 1
  fi
fi

out_dir="$BC_OUT"
if [[ -z "$out_dir" ]]; then
  out_dir="boot-capture_$(date '+%Y%m%d_%H%M%S')"
fi
mkdir -p "$out_dir" || { err "cannot create $out_dir"; exit 1; }

log "Port:     $port"
log "Output:   $out_dir/"
log "Frames:   $BC_FRAMES  (interval ${BC_INTERVAL}s)"
log "Delay:    ${BC_DELAY}s after reset before first frame"
(( BC_RESET )) && reset_label=yes || reset_label=no
(( BC_VIDEO )) && video_label="yes (${BC_FPS} fps → bootup.mp4)" || video_label=no
log "Reset:    $reset_label"
log "Video:    $video_label"
log ""

if (( BC_RESET )); then
  log "Resetting device…"
  reset_device "$port" || exit $?
  log "Waiting ${BC_DELAY}s for boot…"
  sleep "$BC_DELAY"
fi

failed=0
for (( i = 1; i <= BC_FRAMES; i++ )); do
  frame="$(printf '%02d' "$i")"
  outfile="${out_dir}/frame_${frame}.png"
  log "[$i/$BC_FRAMES] → frame_${frame}.png"
  if ! python3 "$SCREENSHOT" "$port" "$outfile"; then
    warn "frame $frame failed"
    (( failed++ )) || true
  fi
  if (( i < BC_FRAMES )); then
    sleep "$BC_INTERVAL"
  fi
done

captured=0
for f in "${out_dir}"/frame_*.png(N); do
  (( captured++ )) || true
done

log ""
if (( captured == 0 )); then
  err "no frames captured in $out_dir"
  exit 1
fi

log "Captured $captured PNG(s) in $out_dir/"
(( failed > 0 )) && warn "$failed frame(s) failed"

if (( BC_VIDEO )); then
  mp4="${out_dir}/bootup.mp4"
  log "Assembling $mp4 at ${BC_FPS} fps…"
  if ffmpeg -y -hide_banner -loglevel warning \
      -framerate "$BC_FPS" \
      -i "${out_dir}/frame_%02d.png" \
      -pix_fmt yuv420p \
      "$mp4"; then
    log "Done → $mp4"
  else
    err "ffmpeg failed (missing frame numbers in sequence?)"
    exit 1
  fi
fi
