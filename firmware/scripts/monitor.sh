#!/usr/bin/env bash
# Open the serial monitor for the Glane Notes board.
#
# Usage:
#   ./scripts/monitor.sh
#   ./scripts/monitor.sh -p /dev/cu.xxx
#
# Exit the monitor with Ctrl-].

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

ARG_PORT=""
while [ $# -gt 0 ]; do
    case "$1" in
        -p|--port) ARG_PORT="$2"; shift 2 ;;
        *) echo "Unknown arg: $1" >&2; exit 2 ;;
    esac
done
[ -n "${ARG_PORT}" ] && PORT="${ARG_PORT}"

ensure_idf
cd "${PROJECT_DIR}"

if ! PORT_RESOLVED="$(detect_port)"; then
    echo "ERROR: no serial port found. Pass -p <port>." >&2
    exit 1
fi
echo ">> Monitoring ${PORT_RESOLVED} (Ctrl-] to quit) ..."
idf.py -p "${PORT_RESOLVED}" monitor
