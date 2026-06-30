#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_DIR"

export IDF_PYTHON_CHECK_CONSTRAINTS="${IDF_PYTHON_CHECK_CONSTRAINTS:-no}"

if [[ -n "${IDF_PATH:-}" ]]; then
  if [[ -z "${PYTHON_BIN:-}" && -n "${IDF_PYTHON_ENV_PATH:-}" ]]; then
    PYTHON_BIN="$IDF_PYTHON_ENV_PATH/bin/python3"
  fi
  PYTHON_BIN="${PYTHON_BIN:-python3}"
  if [[ -d "$HOME/.platformio/packages/toolchain-xtensa-esp-elf/bin" ]]; then
    export PATH="$HOME/.platformio/packages/toolchain-xtensa-esp-elf/bin:$PATH"
  fi
  export PYTHONPATH="$IDF_PATH/tools:$IDF_PATH/components/partition_table:$IDF_PATH/components/esp_rom:$IDF_PATH/tools/esp_app_trace:$IDF_PATH/tools/idf_monitor_base${PYTHONPATH:+:$PYTHONPATH}"
  exec "$PYTHON_BIN" "$IDF_PATH/tools/idf.py" -B build "$@"
fi

if command -v idf.py >/dev/null 2>&1; then
  exec idf.py -B build "$@"
fi

PIO_IDF_PATH="$HOME/.platformio/packages/framework-espidf"
PIO_PYTHON="$HOME/.platformio/penv/bin/python3"
if [[ "${WEC_ALLOW_PLATFORMIO_IDF:-0}" == "1" && -x "$PIO_PYTHON" && -f "$PIO_IDF_PATH/tools/idf.py" ]]; then
  export IDF_PATH="$PIO_IDF_PATH"
  export PYTHON_BIN="$PIO_PYTHON"
  export PATH="$HOME/.platformio/packages/toolchain-xtensa-esp-elf/bin:$PATH"
  export PYTHONPATH="$IDF_PATH/tools:$IDF_PATH/components/partition_table:$IDF_PATH/components/esp_rom:$IDF_PATH/tools/esp_app_trace:$IDF_PATH/tools/idf_monitor_base"
  exec "$PYTHON_BIN" "$IDF_PATH/tools/idf.py" -B build "$@"
fi

echo "ESP-IDF not found. Source export.sh or set IDF_PATH/PYTHON_BIN first." >&2
echo "Set WEC_ALLOW_PLATFORMIO_IDF=1 only if you intentionally want the local PlatformIO-packaged ESP-IDF fallback." >&2
exit 1
