#!/usr/bin/env bash
set -euo pipefail

PORT="${1:-${PORT:-}}"
BAUD="${BAUD:-115200}"

if [[ -z "${PORT}" ]]; then
  echo "Usage: $0 /dev/ttyUSB0" >&2
  echo "Or set PORT env var." >&2
  exit 1
fi

arduino-cli monitor -p "$PORT" -c "baudrate=${BAUD}"
