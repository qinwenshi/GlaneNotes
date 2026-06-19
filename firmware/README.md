# Glane Notes — ESP-IDF Firmware

A voice-recorder note device for the **Waveshare ESP32-S3-ePaper-1.54** board
(ESP32-S3-PICO-1, 8 MB flash, 8 MB PSRAM). Inspired by "Pala Note".

Press a button → record your voice to the SD card as WAV. When you sync over
Wi-Fi, each recording is uploaded once over HTTPS to **Aliyun DashScope
(`qwen3-asr-flash`)**, transcribed, and the text is written back next to the
audio on the SD card. Browse everything from a small built-in web dashboard.

> Audio stays **local on the SD card by default**. It is only uploaded when you
> explicitly request a network sync. Transcription is **file-based (one HTTPS
> POST per recording)** — no WebSocket / realtime streaming.

## Hardware

| Function | Pin |
|---|---|
| E-paper (SSD1681 200×200) | MOSI 13, SCK 12, CS 11, DC 10, RST 9, BUSY 8, PWR 6 (low=on) |
| SD card (SDMMC 1-bit) | CLK 39, CMD 41, D0 40 |
| I2S audio | MCLK 14, BCLK 15, LRC 38, DOUT 45, DIN 16 (mic) |
| Codec ES8311 (I2C) | SDA 47, SCL 48 |
| Power rails | AUDIO_PWR 42 (low=on), PA 46 (high=on), VBAT_LATCH 17 |
| Battery sense | GPIO4 (ADC1_CH3), ~1/2 divider (×2.02) |
| Buttons | BOOT 0, PWR 18 |

All pins are defined in [`main/glane_config.h`](main/glane_config.h).

## Recording format

16 kHz · 16-bit · mono WAV. The codec and the I2S mic capture both run at
16 kHz so no resampling is needed. Files are stored at `/sdcard/notes/note-<ts>.wav`
and transcripts at `/sdcard/notes/note-<ts>.txt`.

## Audio feedback

Short tones play through the ES8311 DAC + speaker as feedback:

- **Recording start** — a rising two-note cue (A5 → E6).
- **Recording stop** — a falling two-note cue (E6 → A5).

The amplifier (`PA_PIN` 46) and DAC are normally off/muted; they are switched on
only for the brief cue and turned back off afterwards. Tones are generated as
fade-in/out sine waves on I2S0 (see [`main/tone.cpp`](main/tone.cpp)) and only
play while idle, never during capture.

## Build & flash

Requires ESP-IDF **v5.5+**.

### Helper scripts (recommended)

Convenience wrappers live in [`scripts/`](scripts/). They auto-source ESP-IDF
and auto-detect the serial port:

```bash
./scripts/build.sh              # set-target (first run) + build
./scripts/build.sh clean        # idf.py fullclean, then build
./scripts/build.sh fullclean    # rm -rf build/, then build

./scripts/flash.sh              # build + flash (auto-detect port)
./scripts/flash.sh --monitor    # flash, then open the serial monitor
./scripts/flash.sh -p /dev/cu.usbmodemXXXX

./scripts/monitor.sh            # just open the serial monitor (Ctrl-] to quit)
```

Override the ESP-IDF location or port via env vars:

```bash
IDF_DIR=/path/to/esp-idf ./scripts/build.sh
PORT=/dev/cu.usbmodem1101 ./scripts/flash.sh
```

`IDF_DIR` defaults to `/Users/shiqinwen/opensource/esp-idf`; change it to match
your machine.

### Manual (plain idf.py)

```bash
source /path/to/esp-idf/export.sh
cd firmware
idf.py set-target esp32s3      # first time only
idf.py build
idf.py -p /dev/tty.usbmodemXXXX flash monitor
```

The board enumerates as a USB CDC serial device (console runs over USB-Serial-JTAG).

## Configuration (Wi-Fi + API key)

There is no hard-coded Wi-Fi or API key — everything is stored in NVS and set
from the built-in web dashboard. On a brand-new device (no Wi-Fi saved yet) the
firmware automatically starts a **setup access point** so you can configure it
with nothing but a phone:

1. Power on. The screen shows **WIFI SETUP** with an SSID and URL.
2. On your phone, join the Wi-Fi network **`GlaneNotes-Setup`** (open, no password).
3. Open **`http://192.168.4.1`** in a browser — the Settings page opens.
4. Enter your **Wi-Fi SSID + password** and your **DashScope API key**, then Save.
5. The device shows **SAVED / Restarting** and reboots onto your network.

You can re-enter setup any time: from the home screen, **hold BOOT** to sync; if
no Wi-Fi is configured it reopens the setup AP. Press any button to cancel setup.

You need:

- **Wi-Fi SSID / password** — to reach the internet for sync.
- **DashScope API key** — an Aliyun Bailian (百炼) API key with access to
  `qwen3-asr-flash`. Get it from the Aliyun Bailian console.

> **Offline-first:** Wi-Fi never blocks startup. When credentials are saved the
> device tries to connect in the background; if it fails (e.g. you're out and
> about) recording, list and playback keep working fully offline. Sync simply
> reports *Working offline* and returns home.

### DashScope request used

`POST https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions`

```json
{
  "model": "qwen3-asr-flash",
  "messages": [
    {"role": "user", "content": [
      {"type": "input_audio",
       "input_audio": {"data": "data:audio/wav;base64,<BASE64>"}}
    ]}
  ],
  "stream": false,
  "asr_options": {"enable_itn": true}
}
```

Headers: `Authorization: Bearer <API_KEY>`, `Content-Type: application/json`.
The transcript is read from `choices[0].message.content`. Inline base64 audio is
capped at 3 MB per file (`MAX_WAV_BYTES` in `transcribe.cpp`), well under the
DashScope 10 MB limit — roughly 90 s of 16 kHz mono audio.

## Usage

The device is a small state machine driven by two buttons. Context determines
what each press does.

### Home (idle)

| Action | Result |
|---|---|
| **Short-press BOOT** | Start recording (rising tone). Press again to stop. |
| **Long-press BOOT** | Trigger a Wi-Fi sync (transcribe all un-transcribed notes) |
| **Short-press PWR** | Open the on-device **notes list** |
| **Long-press PWR** (≥0.8 s) | Enter deep sleep immediately |
| Idle for a while | Device enters deep sleep; press BOOT to wake |

### Notes list

| Action | Result |
|---|---|
| **Short-press PWR** | Move the selection cursor down (wraps to top) |
| **Short-press BOOT** | Open the selected note's **detail** screen |
| **Long-press BOOT** | Back to the home screen |
| **Long-press PWR** | Deep sleep |

### Note detail

| Action | Result |
|---|---|
| **Short-press BOOT** | Play the recording through the speaker |
| **Short-press PWR** | Back to the notes list |
| **Long-press BOOT** | Back to the home screen |

### Playing

| Action | Result |
|---|---|
| **Short-press BOOT / PWR** | Stop playback |
| (playback finishes) | Returns automatically to the detail screen |

Notes are shown newest-first. Each row shows its number and, once the clock has
been set over Wi-Fi (SNTP), the recording date/time (`MM-DD HH:MM`); before the
first sync the time shows as `--:--`. Note IDs come from a persistent NVS counter
so they never collide across deep-sleep reboots or power cycles.

The home screen shows a **battery indicator** (icon + percentage) in the top-left,
read from GPIO4 (ADC1_CH3) through the on-board ~1/2 divider (×2.02), plus the
note count and Wi-Fi status.

### Display refresh

The e-paper uses **fast partial refresh** for in-screen updates (recording timer,
list cursor movement) and only performs a **full flash refresh** when switching
between different screen kinds or every 30 partial updates (to clear ghosting).
This keeps the UI feeling responsive while preventing artifact build-up.

The e-ink screen shows English status text (idle / recording / syncing / messages).
Chinese (or any) transcripts are viewable in the web dashboard and the `.txt`
files on the SD card.

### Web dashboard

When connected to Wi-Fi the device serves a dashboard on its IP:

- `/` — list of notes with size & transcript status
- `/note?id=...` — view a transcript
- `/dl?id=...` — download the WAV
- `/del?id=...` — delete a note (+ its transcript)
- `/settings` — set Wi-Fi credentials & DashScope API key
- `/sync` — request a sync (returns immediately; runs when idle)

## Module map (`main/`)

| File | Responsibility |
|---|---|
| `glane_config.h` | All pins & constants (single source of truth) |
| `app_main.cpp` | Power init + state machine (IDLE / RECORDING / SYNCING / LIST / DETAIL / PLAYING / PROVISION) |
| `recorder.cpp` | Mic WAV capture (I2S0 master TX silence + I2S1 slave RX) |
| `player.cpp` | WAV playback through the speaker (I2S0 TX + ES8311 DAC) |
| `tone.cpp` | Record start/stop feedback tones via ES8311 DAC |
| `battery.cpp` | Battery voltage via ADC1_CH3 (GPIO4) + on-chip calibration |
| `timesync.cpp` | SNTP clock + timestamp formatting for note dates |
| `codec.cpp` | ES8311 codec init (reused from reference player) |
| `sdcard.cpp` | SDMMC mount + free/total bytes |
| `settings.cpp` | NVS store for Wi-Fi creds + DashScope key + note counter |
| `wifi_mgr.cpp` | Wi-Fi STA + SoftAP provisioning manager |
| `transcribe.cpp` | File-based DashScope ASR over HTTPS |
| `sync.cpp` | Scan notes, transcribe those missing a `.txt` |
| `webserver.cpp` | Local `esp_http_server` dashboard |
| `ui.cpp` + `font5x7.h` | E-ink screens (idle/list/detail/playing) w/ partial refresh |
| `buttons.cpp` | Button debounce / long-press detection (reused) |

Drivers `epaper_bsp` (component), `codec`, and `buttons` are reused unchanged
from the `ESP32-S3-EPaper-Player/player_idf` reference project for the same board.

## Notes & limitations

- Note IDs come from a persistent NVS counter (`settings_next_note_seq`), unique
  across reboots and power cycles. Recording dates require an SNTP sync; before
  the first Wi-Fi sync the clock is unset and the list shows `--:--`.
- The dual-controller full-duplex mic path (I2S0 + I2S1) and the DashScope inline
  base64 upload are correctness-by-construction; verify on real hardware.
- The DAC feedback-tone path (`tone.cpp`) is likewise unverified on hardware —
  check tone volume / PA pop behaviour and adjust `TONE_AMPLITUDE` if needed.
- No LVGL — the UI draws directly into the e-ink buffer via `epaper_bsp`.
