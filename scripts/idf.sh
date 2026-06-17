#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_DIR"

export IDF_PATH="${IDF_PATH:-$HOME/.platformio/packages/framework-espidf}"
export IDF_PYTHON_ENV_PATH="${IDF_PYTHON_ENV_PATH:-$HOME/.platformio/penv}"
export PYTHON_BIN="${PYTHON_BIN:-$IDF_PYTHON_ENV_PATH/bin/python3}"
export IDF_PYTHON_CHECK_CONSTRAINTS="${IDF_PYTHON_CHECK_CONSTRAINTS:-no}"
export PATH="$HOME/.platformio/packages/toolchain-xtensa-esp-elf/bin:$PATH"
export PYTHONPATH="$IDF_PATH/tools:$IDF_PATH/components/partition_table:$IDF_PATH/components/esp_rom:$IDF_PATH/tools/esp_app_trace:$IDF_PATH/tools/idf_monitor_base"

exec "$PYTHON_BIN" "$IDF_PATH/tools/idf.py" -B build "$@"
