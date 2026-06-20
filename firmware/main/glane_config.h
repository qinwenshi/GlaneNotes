// glane_config.h — Glane Notes hardware pin map & constants
// Target board: Waveshare ESP32-S3-ePaper-1.54 (ESP32-S3-PICO-1, 8MB flash, 8MB PSRAM)
#pragma once

// ── e-Paper (SSD1681 200x200, SPI2) ──────────────────────────────────────────
#define EPD_MOSI    13
#define EPD_SCK     12
#define EPD_CS      11
#define EPD_DC      10
#define EPD_RST      9
#define EPD_BUSY     8
#define EPD_PWR      6   // active LOW
#define EPD_W      200
#define EPD_H      200

// ── SD card (SDMMC 1-bit) ────────────────────────────────────────────────────
#define SD_CLK      39
#define SD_CMD      41
#define SD_D0       40

// ── I2S audio ────────────────────────────────────────────────────────────────
#define I2S_MCLK    14
#define I2S_BCLK    15
#define I2S_LRC     38   // WS
#define I2S_DOUT    45   // DAC (unused while recording, kept clocked w/ silence)
#define I2S_DIN     16   // mic ADC input

// ── ES8311 codec (I2C) ───────────────────────────────────────────────────────
#define CODEC_SDA   47
#define CODEC_SCL   48

// ── Power rails ──────────────────────────────────────────────────────────────
#define AUDIO_PWR   42   // active LOW
#define PA_PIN      46   // amp enable, active HIGH
#define VBAT_LATCH  17   // hold power on

// ── Battery sense ────────────────────────────────────────────────────────────
// Battery voltage on GPIO4 (ADC1_CH3) via a ~1/2 resistor divider. The measured
// ADC voltage is multiplied by BAT_DIVIDER to recover the pack voltage. Values
// taken from the Waveshare ESP32-S3-ePaper-1.54 reference design.
#define BAT_ADC_GPIO   4
#define BAT_DIVIDER    2.02f
#define BAT_FULL_MV    4200
#define BAT_EMPTY_MV   3300

// ── Buttons ──────────────────────────────────────────────────────────────────
#define BOOT_BTN     0
#define PWR_BTN     18

// ── Recording format ─────────────────────────────────────────────────────────
#define REC_SAMPLE_RATE   16000   // 16 kHz output WAV, matches DashScope ASR
#define REC_CAPTURE_RATE  48000   // ES8311/I2S run at 48 kHz (proven mic rate),
                                  // then decimated /3 to REC_SAMPLE_RATE. The
                                  // ADC word only aligns cleanly in an I2S lane
                                  // at this rate; 16 kHz capture yields fragments.
#define REC_DECIM         (REC_CAPTURE_RATE / REC_SAMPLE_RATE)  // = 3
#define REC_BITS          16
#define REC_CHANNELS      1
#define REC_MAX_SECONDS   600     // hard cap (~19 MB) to avoid runaway files
#define REC_SW_GAIN       2       // software gain (analog PGA does the heavy lift)
#define REC_MIC_PGA_GAIN  4       // ES8311 analog PGA gain 0-7 (lower = headroom,
                                  // avoids ADC-stage clipping that sounds harsh)

// ── Capture startup priming (recover the intermittent dead-ADC case) ─────────
// After deep-sleep wake or a playback session the ES8311 has been seen with its
// analog power register (0x0D) drifted to 0x02, leaving the ADC delivering pure
// digital silence. At record start we settle the analog path with clocks live,
// flush stale DMA, then probe; if the mic is digital-silent we re-assert codec
// power + bounce the I2S RX channel and retry. Runs only at startup, before any
// audio is written — the steady capture loop never recovers mid-recording.
#define REC_SETTLE_MS       250   // analog VMID/bias settle (clocks already live)
#define REC_PRIME_RETRIES   3     // dead-ADC recovery attempts before giving up
#define REC_SILENCE_FLOOR   16    // AC RMS below this on ALL candidates = dead ADC
                                  // (a live mic noise floor measures ~64)

// ── Filesystem layout ────────────────────────────────────────────────────────
#define SD_MOUNT          "/sdcard"
#define NOTES_DIR         "/sdcard/notes"

// ── Power management ─────────────────────────────────────────────────────────
#define IDLE_SLEEP_TIMEOUT_MS   (3UL * 60UL * 1000UL)
