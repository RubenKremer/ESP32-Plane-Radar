#!/usr/bin/env bash
# Flash plane-radar-merged.bin to an ESP32-C3 Super Mini via esptool.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV="${PIOENV:-supermini}"
PORT=""
BAUD=921600
BUILD=0
FIRMWARE=""

usage() {
  cat <<'EOF'
Usage: scripts/flash-firmware.sh [options]

Flash the merged Plane Radar image (bootloader + partitions + app at offset 0x0).

  -p, --port DEV   Serial port (default: auto-detect USB ACM device)
  -f, --firmware   .bin file (default: release/plane-radar-merged.bin)
  --baud RATE      Upload baud rate (default: 921600)
  --build          Build and merge firmware before flashing
  --env NAME       PlatformIO env for --build (default: supermini)
  -h, --help       Show this help

If flashing fails, put the board in download mode: hold BOOT, tap RESET, release BOOT.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -p|--port) PORT="$2"; shift 2 ;;
    -f|--firmware) FIRMWARE="$2"; shift 2 ;;
    --baud) BAUD="$2"; shift 2 ;;
    --build) BUILD=1; shift ;;
    --env) ENV="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
  esac
done

find_python() {
  if command -v python3 >/dev/null 2>&1; then
    echo python3
    return 0
  fi
  return 1
}

find_pio() {
  local candidate
  for candidate in \
    "${HOME}/.local/bin/pio" \
    "${HOME}/.platformio/penv/bin/pio"; do
    if [[ -x "$candidate" ]] && "$candidate" --version >/dev/null 2>&1; then
      echo "$candidate"
      return 0
    fi
  done
  if command -v pio >/dev/null 2>&1 && pio --version >/dev/null 2>&1; then
    echo pio
    return 0
  fi
  return 1
}

find_esptool() {
  local python="$1"
  local candidate="${HOME}/.platformio/packages/tool-esptoolpy/esptool.py"
  if [[ -f "$candidate" ]]; then
    echo "$python" "$candidate"
    return 0
  fi
  if command -v esptool.py >/dev/null 2>&1; then
    echo "$python" "$(command -v esptool.py)"
    return 0
  fi
  if command -v esptool >/dev/null 2>&1; then
    echo esptool
    return 0
  fi
  if "$python" -m esptool version >/dev/null 2>&1; then
    echo "$python" -m esptool
    return 0
  fi
  return 1
}

detect_port() {
  local pio
  if pio="$(find_pio)"; then
    local detected
    detected="$("$pio" device list 2>/dev/null | awk '
      /^\/dev\/ttyACM/ { port = $1; getline; getline; id = $0; getline; getline; desc = $0;
        if (id ~ /303A:/ || desc ~ /Espressif|JTAG|serial debug/) { print port; exit }
        if (port != "") { last = port }
      }
      END { if (NR && last != "") print last }')"
    if [[ -n "$detected" ]]; then
      echo "$detected"
      return 0
    fi
  fi

  local dev
  for dev in /dev/ttyACM* /dev/ttyUSB*; do
    if [[ -e "$dev" ]]; then
      echo "$dev"
      return 0
    fi
  done
  return 1
}

resolve_firmware() {
  if [[ -n "$FIRMWARE" ]]; then
    echo "$FIRMWARE"
    return 0
  fi
  if [[ -f "${ROOT}/release/plane-radar-merged.bin" ]]; then
    echo "${ROOT}/release/plane-radar-merged.bin"
    return 0
  fi
  local built="${ROOT}/.pio/build/${ENV}/firmware-merged.bin"
  if [[ -f "$built" ]]; then
    echo "$built"
    return 0
  fi
  return 1
}

PYTHON="$(find_python || true)"
if [[ -z "$PYTHON" ]]; then
  echo "python3 not found" >&2
  exit 1
fi

ESPTOOL=()
if read -r -a ESPTOOL <<< "$(find_esptool "$PYTHON" || true)"; then
  :
else
  cat >&2 <<'EOF'
esptool not found.

Install PlatformIO and build once (pulls in esptool):
  pipx install platformio
  ./scripts/merge-firmware.sh

Or install esptool directly:
  pipx install esptool
EOF
  exit 1
fi

cd "$ROOT"

if [[ "$BUILD" -eq 1 ]]; then
  "${ROOT}/scripts/merge-firmware.sh" --env "$ENV"
fi

if ! FIRMWARE="$(resolve_firmware)"; then
  echo "Firmware image not found." >&2
  echo "Run ./scripts/merge-firmware.sh first, or pass -f PATH." >&2
  exit 1
fi

if [[ -z "$PORT" ]]; then
  if ! PORT="$(detect_port)"; then
    cat >&2 <<'EOF'
No serial port detected.

Connect the ESP32-C3 over USB and pass the port explicitly:
  ./scripts/flash-firmware.sh -p /dev/ttyACM0
EOF
    exit 1
  fi
fi

if [[ ! -e "$PORT" ]]; then
  echo "Serial port not found: $PORT" >&2
  exit 1
fi

if [[ ! -f "$FIRMWARE" ]]; then
  echo "Firmware file not found: $FIRMWARE" >&2
  exit 1
fi

echo "Flashing ${FIRMWARE}"
echo "  chip:     esp32c3"
echo "  port:     ${PORT}"
echo "  baud:     ${BAUD}"
echo "  offset:   0x0"
echo "  flash:    4MB @ 80MHz"

"${ESPTOOL[@]}" --chip esp32c3 --port "$PORT" --baud "$BAUD" \
  write_flash \
  --flash_mode keep \
  --flash_freq 80m \
  --flash_size 4MB \
  0x0 "$FIRMWARE"

echo "Done. Reset the board or unplug/replug USB to run the new firmware."
