#!/usr/bin/env bash
set -euo pipefail

SKETCH_DIR="${SKETCH_DIR:-/home/emaj/git/monni/arduino/monni_pro}"
FQBN="${FQBN:-arduino:avr:mega}"
PORT="${1:-${PORT:-}}"

if [[ -z "${PORT}" ]]; then
  echo "Usage: $0 /dev/ttyUSB0" >&2
  echo "Or set PORT env var." >&2
  exit 1
fi

arduino-cli compile --fqbn "$FQBN" "$SKETCH_DIR"
arduino-cli upload --fqbn "$FQBN" -p "$PORT" "$SKETCH_DIR"
