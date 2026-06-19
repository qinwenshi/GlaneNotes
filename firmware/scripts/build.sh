#!/usr/bin/env bash
# Build the Glane Notes firmware.
#
# Usage:
#   ./scripts/build.sh            # build (sets target to esp32s3 on first run)
#   ./scripts/build.sh clean      # full clean, then build
#   ./scripts/build.sh fullclean  # remove build/ entirely, then build
#
# Env overrides:
#   IDF_DIR=/path/to/esp-idf   ESP-IDF checkout (default: local opensource path)

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

ensure_idf
cd "${PROJECT_DIR}"

case "${1:-}" in
    fullclean)
        echo ">> Removing build/ ..."
        rm -rf build
        ;;
    clean)
        echo ">> idf.py fullclean ..."
        idf.py fullclean || true
        ;;
esac

# set-target is idempotent but only needed once; run it if no sdkconfig yet.
if [ ! -f sdkconfig ]; then
    echo ">> Setting target esp32s3 ..."
    idf.py set-target esp32s3
fi

echo ">> Building ..."
idf.py build

echo ">> Done. Binary: ${PROJECT_DIR}/build/glane_notes.bin"
