# Glane Notes — Offline-First Voice Note Recorder on ESP32-S3 + E-Paper

> A pocketable "second brain" voice recorder: **press to record, release to save**.
> Capture fleeting ideas in seconds, fully offline — transcribe to text later via
> the cloud, on your terms.

**Glane Notes** is open-source [ESP-IDF](https://github.com/espressif/esp-idf)
firmware that turns a **Waveshare ESP32-S3-ePaper-1.54** board into a tiny,
distraction-free voice memo device with an e-ink display. Inspired by the
"Pala Note" project, it is designed around one idea: *capture now, process later*.

Hold a button to record straight to the SD card as WAV; let go and it saves
instantly with an audible cue, then drops back into ultra-low-power deep sleep.
When you choose to sync over Wi-Fi, recordings are transcribed to searchable text
using **Aliyun DashScope (Bailian) `qwen3-asr-flash`** and written back next to
the audio. Browse, play, and export everything from an on-device list or a small
built-in web dashboard.

<!-- Keywords: ESP32-S3 voice recorder, e-paper note taker, e-ink voice memo,
ESP-IDF firmware, speech-to-text, ASR transcription, Aliyun DashScope qwen3-asr-flash,
Waveshare ESP32-S3-ePaper-1.54, SD card audio recorder, offline voice notes,
second brain device, DIY voice recorder, SSD1681 e-paper, ES8311 codec. -->

---

## ✨ Why Glane Notes is different

- **🔌 Offline-first capture.** Recording, the on-device note list, and speaker
  playback are **100% local**. Wi-Fi never blocks startup — out and about or with
  no network at all, the device works fully. The network is a bonus, not a
  prerequisite.
- **☁️ Deferred, on-demand transcription.** Audio stays on the SD card by default
  and is uploaded **only when you explicitly sync**. Transcription is
  **file-based (one HTTPS POST per recording)** to Aliyun DashScope
  `qwen3-asr-flash` — no WebSocket, no realtime streaming, no OpenAI Whisper
  dependency. Every note ends up as **both a playable recording and searchable
  text**.
- **📶 Zero-friction Wi-Fi setup.** A brand-new device auto-hosts a
  `GlaneNotes-Setup` access point. Join it from your phone, open `192.168.4.1`,
  and enter your Wi-Fi credentials **and** DashScope API key on one page — solving
  the classic "you need a network to configure the network" chicken-and-egg
  problem.
- **🖥️ Low-flicker e-ink UI.** Screen transitions use fast **partial refresh**; a
  single-flash full refresh runs only every 60 updates to clear ghosting. Paging
  feels almost instant on an e-paper panel.
- **🔋 Genuinely portable.** GPIO power latch, automatic deep sleep after idle,
  and an on-screen **battery indicator** (calibrated ADC) make it an everyday
  carry on a small 500 mAh LiPo.
- **🔁 Reproducible & hackable.** Pure native ESP-IDF, clean modular C++,
  one-command build/flash scripts, and committed `dependencies.lock` — fork it
  and build your own.

## 🎬 How it works

1. **Hold BOOT** → recording starts (rising tone). Speak your note.
2. **Release / press again** → saves to `/sdcard/notes/` as 16 kHz mono WAV
   (falling tone), then returns to the low-power home screen.
3. **Press PWR** → open the on-device notes list; **BOOT** opens a note,
   **BOOT again** plays it through the speaker.
4. **Hold BOOT** → sync over Wi-Fi: each un-transcribed note is uploaded to
   DashScope and a `.txt` transcript is written back beside the audio.
5. Or open the **web dashboard** (the device's IP) to browse, play, and download
   notes and transcripts from your phone or laptop.

## 🧩 Hardware

Target board: **Waveshare ESP32-S3-ePaper-1.54** (ESP32-S3-PICO-1, dual-core
240 MHz, 8 MB flash, 8 MB PSRAM).

| Function | Detail |
|---|---|
| MCU | ESP32-S3 (Wi-Fi + Bluetooth LE) |
| Display | 1.54" SSD1681 e-paper, 200×200, partial refresh |
| Audio | ES8311 codec, I2S mic capture + speaker playback |
| Storage | microSD (SDMMC), all notes + transcripts on card |
| Power | LiPo (e.g. 500 mAh), GPIO latch, deep sleep, battery ADC |
| Input | BOOT + PWR buttons |

Full pin map: [`firmware/main/glane_config.h`](firmware/main/glane_config.h).

## 🚀 Build & flash

Requires **ESP-IDF v5.5+**. Convenience scripts auto-source ESP-IDF and
auto-detect the serial port:

```bash
cd firmware
./scripts/build.sh        # set-target (first run) + build
./scripts/flash.sh        # flash over USB (auto-detect port)
./scripts/monitor.sh      # serial monitor
```

See [`firmware/README.md`](firmware/README.md) for the full build, flashing,
configuration, web dashboard, and architecture documentation.

## ⚙️ First-time configuration

No Wi-Fi or API key is hard-coded. On first boot the device opens the
`GlaneNotes-Setup` Wi-Fi hotspot:

1. Join **`GlaneNotes-Setup`** from your phone (open network).
2. Open **`http://192.168.4.1`** in a browser.
3. Enter your **Wi-Fi SSID + password** and **Aliyun DashScope API key**, then Save.
4. The device reboots onto your network and is ready to sync.

You need an [Aliyun Bailian (百炼)](https://bailian.console.aliyun.com/) API key
with access to the `qwen3-asr-flash` speech-recognition model.

## 🗂️ Project structure

```
firmware/
├── main/            # application: recorder, player, UI, sync, web, wifi, battery…
├── components/      # epaper_bsp (SSD1681 driver)
├── scripts/         # build / flash / monitor helpers
└── README.md        # detailed firmware documentation
```

## 🛣️ Roadmap ideas

- Tag menu after recording (project / todo / idea)
- Automatic once-per-day background sync
- Tag filtering & full-text search in the web dashboard
- On-device note summaries / titles via the existing AI connection

## 📄 License & credits

Built with [ESP-IDF](https://github.com/espressif/esp-idf). E-paper, codec, and
button drivers are adapted from the `ESP32-S3-EPaper-Player` reference project for
the same board. Concept inspired by the "Pala Note" pocket voice recorder.

Contributions, forks, and feature ideas are welcome.
