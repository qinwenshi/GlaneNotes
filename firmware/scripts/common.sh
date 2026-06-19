#!/usr/bin/env bash
# Shared helpers for Glane Notes build/flash scripts.
# Sourced by build.sh / flash.sh / monitor.sh.

set -euo pipefail

# Resolve the firmware project root (parent of this scripts/ dir).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

# IDF_DIR can be overridden via env. Default to the repo's local checkout.
IDF_DIR="${IDF_DIR:-/Users/shiqinwen/opensource/esp-idf}"

ensure_idf() {
    # Already exported in this shell? Reuse it.
    if command -v idf.py >/dev/null 2>&1 && [ -n "${IDF_PATH:-}" ]; then
        return 0
    fi
    if [ ! -f "${IDF_DIR}/export.sh" ]; then
        echo "ERROR: ESP-IDF not found at: ${IDF_DIR}" >&2
        echo "Set IDF_DIR to your ESP-IDF checkout, e.g.:" >&2
        echo "  IDF_DIR=/path/to/esp-idf $0" >&2
        exit 1
    fi
    # shellcheck disable=SC1091
    source "${IDF_DIR}/export.sh" >/dev/null
}

# Auto-detect a likely ESP32-S3 serial port if PORT is not set.
detect_port() {
    if [ -n "${PORT:-}" ]; then
        echo "${PORT}"
        return 0
    fi
    local p
    for p in /dev/cu.usbmodem* /dev/cu.wchusbserial* /dev/cu.SLAB_USBtoUART* \
             /dev/ttyUSB* /dev/ttyACM*; do
        if [ -e "$p" ]; then
            echo "$p"
            return 0
        fi
    done
    return 1
}
