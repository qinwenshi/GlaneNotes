#!/usr/bin/env bash
# mic_debug.sh — capture serial and surface the recorder's per-lane mic RMS.
#
# Usage:  ./scripts/mic_debug.sh [-p PORT] [-s SECONDS]
# Then hold the record button and speak. Each 500 ms the firmware prints:
#   mic lane RMS  L0=.. L1=.. L2=.. L3=..  (using lane N)
#
# Interpretation:
#   * One lane clearly larger while you speak  -> mic capture WORKS (that lane
#     is selected automatically; you should hear audio on playback).
#   * All four lanes ~0 even while speaking     -> ES8311 ADC is delivering
#     digital silence: a codec/analog problem, not the I2S extraction.
# The one-shot "0xNN: 0xVV" dump at record start is the ES8311 register state
# (check 0x14=0x5A mic-on, 0x17=0xC8 ADC vol, 0x16 PGA gain).

set -euo pipefail
PORT=""
SECS=60
while [ $# -gt 0 ]; do
  case "$1" in
    -p) PORT="$2"; shift 2;;
    -s) SECS="$2"; shift 2;;
    *)  echo "unknown arg: $1"; exit 1;;
  esac
done
if [ -z "$PORT" ]; then
  PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)
fi
if [ -z "$PORT" ]; then
  echo "No serial port found (set with -p). Is the device awake/plugged in?"
  exit 1
fi
echo "Listening on $PORT for ${SECS}s — hold the record button and speak..."
python3 - "$PORT" "$SECS" <<'PY'
import sys, time, serial
port, secs = sys.argv[1], int(sys.argv[2])
s = serial.Serial(port, 115200, timeout=1)
end = time.time() + secs
while time.time() < end:
    line = s.readline().decode(errors='replace').rstrip()
    if not line:
        continue
    if ('mic lane RMS' in line) or line[2:4] == ': ' or 'recording' in line \
       or 'PCM bytes' in line or 'codec' in line:
        print(line, flush=True)
PY
