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
#define REC_SAMPLE_RATE   16000   // 16 kHz, matches DashScope ASR
#define REC_BITS          16
#define REC_CHANNELS      1
#define REC_MAX_SECONDS   600     // hard cap (~19 MB) to avoid runaway files

// ── Filesystem layout ────────────────────────────────────────────────────────
#define SD_MOUNT          "/sdcard"
#define NOTES_DIR         "/sdcard/notes"

// ── Power management ─────────────────────────────────────────────────────────
#define IDLE_SLEEP_TIMEOUT_MS   (3UL * 60UL * 1000UL)
