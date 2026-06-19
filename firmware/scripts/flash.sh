#!/usr/bin/env bash
# Flash the Glane Notes firmware to the board (builds first if needed).
#
# Usage:
#   ./scripts/flash.sh                 # auto-detect port, build + flash
#   ./scripts/flash.sh -p /dev/cu.xxx  # explicit port
#   ./scripts/flash.sh --monitor       # flash, then open serial monitor
#   PORT=/dev/cu.xxx ./scripts/flash.sh
#
# Env overrides:
#   IDF_DIR=/path/to/esp-idf   ESP-IDF checkout
#   PORT=/dev/cu.usbmodemXXXX  serial port (else auto-detected)

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

DO_MONITOR=0
ARG_PORT=""
while [ $# -gt 0 ]; do
    case "$1" in
        -p|--port) ARG_PORT="$2"; shift 2 ;;
        --monitor|-m) DO_MONITOR=1; shift ;;
        *) echo "Unknown arg: $1" >&2; exit 2 ;;
    esac
done
[ -n "${ARG_PORT}" ] && PORT="${ARG_PORT}"

ensure_idf
cd "${PROJECT_DIR}"

if ! PORT_RESOLVED="$(detect_port)"; then
    echo "ERROR: no serial port found. Plug in the board or pass -p <port>." >&2
    echo "Available ports:" >&2
    ls /dev/cu.* /dev/ttyUSB* /dev/ttyACM* 2>/dev/null >&2 || true
    exit 1
fi
echo ">> Using port: ${PORT_RESOLVED}"

if [ "${DO_MONITOR}" -eq 1 ]; then
    idf.py -p "${PORT_RESOLVED}" flash monitor
else
    idf.py -p "${PORT_RESOLVED}" flash
fi
