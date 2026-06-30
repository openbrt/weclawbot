#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_DIR"

PORT="${1:-/dev/cu.usbmodem21101}"
PYTHON_BIN="${PYTHON_BIN:-/usr/local/bin/python3}"
mkdir -p logs
exec "$PYTHON_BIN" scripts/device_log.py "$PORT" >> logs/device-current.log 2>> logs/device-current.err
