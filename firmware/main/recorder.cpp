// recorder.cpp — onboard-mic WAV recorder.
//
// Capture path mirrors the proven ESP32-S3-EPaper-Player design:
//   • I2S0 master TX clocks the ES8311 (MCLK/BCLK/WS). The DAC output is unused
//     while recording, but TX must stay enabled & fed with silence so the
//     shared bit/word clocks keep running for the slave RX.
//   • I2S1 slave RX captures the mic, sharing I2S0's BCLK/WS. ES8311 ADC audio
//     sits in bits[31:16] of each 32-bit slot, left channel.
// Codec + I2S both run at 16 kHz so captured PCM needs no resampling — it is
// written straight into a 16 kHz/16-bit/mono WAV.

#include "recorder.h"
#include "glane_config.h"
#include "codec.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

extern "C" {
#include "driver/i2s_std.h"
#include "driver/i2s_common.h"
}

static const char *TAG = "recorder";

static i2s_chan_handle_t s_tx = nullptr;  // I2S0 master (clock source, silence)
static i2s_chan_handle_t s_rx = nullptr;  // I2S1 slave  (mic capture)

static volatile bool     s_running = false;
static FILE             *s_file    = nullptr;
static volatile uint32_t s_data_bytes = 0;
static int64_t           s_start_us   = 0;

static TaskHandle_t      s_rec_task = nullptr;
static TaskHandle_t      s_tx_task  = nullptr;
static SemaphoreHandle_t s_rec_done = nullptr;
static SemaphoreHandle_t s_tx_done  = nullptr;

// ── WAV header ───────────────────────────────────────────────────────────────
static void write_wav_header(FILE *f, uint32_t data_len)
{
    uint32_t sample_rate = REC_SAMPLE_RATE;
    uint16_t channels    = REC_CHANNELS;
    uint16_t bits        = REC_BITS;
    uint32_t byte_rate   = sample_rate * channels * bits / 8;
    uint16_t block_align = channels * bits / 8;
    uint32_t riff_len    = 36 + data_len;

    uint8_t h[44];
    memcpy(h + 0,  "RIFF", 4);
    memcpy(h + 4,  &riff_len, 4);
    memcpy(h + 8,  "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    uint32_t fmt_len = 16; memcpy(h + 16, &fmt_len, 4);
    uint16_t fmt_pcm = 1;  memcpy(h + 20, &fmt_pcm, 2);
    memcpy(h + 22, &channels, 2);
    memcpy(h + 24, &sample_rate, 4);
    memcpy(h + 28, &byte_rate, 4);
    memcpy(h + 32, &block_align, 2);
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data_len, 4);
    fwrite(h, 1, 44, f);
}

// ── I2S bring-up ─────────────────────────────────────────────────────────────
static bool i2s_start(void)
{
    // I2S0 master TX — generates MCLK/BCLK/WS for the codec.
    i2s_chan_config_t tx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    tx_cfg.dma_desc_num  = 6;
    tx_cfg.dma_frame_num = 240;
    if (i2s_new_channel(&tx_cfg, &s_tx, nullptr) != ESP_OK) return false;

    i2s_std_config_t tx_std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(REC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                         I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)I2S_MCLK,
            .bclk = (gpio_num_t)I2S_BCLK,
            .ws   = (gpio_num_t)I2S_LRC,
            .dout = (gpio_num_t)I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { false, false, false },
        },
    };
    if (i2s_channel_init_std_mode(s_tx, &tx_std) != ESP_OK) return false;
    if (i2s_channel_enable(s_tx) != ESP_OK) return false;

    // I2S1 slave RX — mic capture, shares I2S0 BCLK/WS.
    i2s_chan_config_t rx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_SLAVE);
    rx_cfg.dma_desc_num  = 8;
    rx_cfg.dma_frame_num = 256;
    if (i2s_new_channel(&rx_cfg, nullptr, &s_rx) != ESP_OK) return false;

    i2s_std_config_t rx_std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(REC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                         I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)I2S_BCLK,
            .ws   = (gpio_num_t)I2S_LRC,
            .dout = I2S_GPIO_UNUSED,
            .din  = (gpio_num_t)I2S_DIN,
            .invert_flags = { false, false, false },
        },
    };
    // 32-bit physical slot to match the master frame; ADC sample in bits[31:16].
    rx_std.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    if (i2s_channel_init_std_mode(s_rx, &rx_std) != ESP_OK) return false;
    if (i2s_channel_enable(s_rx) != ESP_OK) return false;

    return true;
}

static void i2s_stop(void)
{
    if (s_rx) { i2s_channel_disable(s_rx); i2s_del_channel(s_rx); s_rx = nullptr; }
    if (s_tx) { i2s_channel_disable(s_tx); i2s_del_channel(s_tx); s_tx = nullptr; }
}

// ── TX silence task: keep the shared clocks fed ──────────────────────────────
static void tx_silence_task(void *)
{
    const int N = 240;                 // frames per write
    size_t  bytes = N * 2 * sizeof(int32_t);
    int32_t *zeros = (int32_t *)heap_caps_calloc(1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (zeros) {
        while (s_running) {
            size_t w = 0;
            i2s_channel_write(s_tx, zeros, bytes, &w, pdMS_TO_TICKS(100));
        }
        heap_caps_free(zeros);
    }
    xSemaphoreGive(s_tx_done);
    vTaskDelete(nullptr);
}

// ── Recorder task: mic → 16-bit mono → WAV ───────────────────────────────────
static void rec_task(void *)
{
    const int FRAMES = 256;
    size_t  in_bytes = FRAMES * 2 * sizeof(int32_t);   // stereo 32-bit slots
    int32_t *stereo  = (int32_t *)heap_caps_malloc(in_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *mono    = (int16_t *)heap_caps_malloc(FRAMES * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t max_bytes = (uint32_t)REC_SAMPLE_RATE * REC_CHANNELS * (REC_BITS / 8) * REC_MAX_SECONDS;

    if (stereo && mono) {
        while (s_running) {
            size_t got = 0;
            esp_err_t r = i2s_channel_read(s_rx, stereo, in_bytes, &got, pdMS_TO_TICKS(200));
            if (r != ESP_OK || got < sizeof(int32_t) * 2) continue;

            int frames = (int)(got / (sizeof(int32_t) * 2));
            for (int i = 0; i < frames; i++) {
                // Left channel, ADC audio in upper 16 bits of the 32-bit slot.
                mono[i] = (int16_t)(stereo[i * 2] >> 16);
            }
            size_t wlen = (size_t)frames * sizeof(int16_t);
            if (s_file) fwrite(mono, 1, wlen, s_file);
            s_data_bytes += wlen;

            if (s_data_bytes >= max_bytes) {
                ESP_LOGW(TAG, "max recording length reached");
                s_running = false;
                break;
            }
        }
    } else {
        ESP_LOGE(TAG, "rec_task OOM");
    }
    heap_caps_free(stereo);
    heap_caps_free(mono);
    xSemaphoreGive(s_rec_done);
    vTaskDelete(nullptr);
}

// ── Public API ───────────────────────────────────────────────────────────────
void recorder_init(void)
{
    if (!s_rec_done) s_rec_done = xSemaphoreCreateBinary();
    if (!s_tx_done)  s_tx_done  = xSemaphoreCreateBinary();
}

bool recorder_start(const char *vfs_path)
{
    if (s_running) return false;

    s_file = fopen(vfs_path, "wb");
    if (!s_file) {
        ESP_LOGE(TAG, "open %s failed", vfs_path);
        return false;
    }
    // Larger stdio buffer so SD writes are batched, not per-256-sample.
    static char filebuf[8192];
    setvbuf(s_file, filebuf, _IOFBF, sizeof(filebuf));
    write_wav_header(s_file, 0);   // placeholder, patched on stop

    // Codec: 16 kHz, mic on with healthy hardware gain.
    codec_set_sample_rate(REC_SAMPLE_RATE);
    codec_enable_mic(true);
    codec_set_mic_gain(6);
    codec_dac_mute(true);          // speaker silent during capture

    if (!i2s_start()) {
        ESP_LOGE(TAG, "i2s_start failed");
        i2s_stop();
        fclose(s_file);
        s_file = nullptr;
        return false;
    }

    s_data_bytes = 0;
    s_start_us   = esp_timer_get_time();
    s_running    = true;

    xSemaphoreTake(s_tx_done, 0);
    xSemaphoreTake(s_rec_done, 0);
    xTaskCreatePinnedToCore(tx_silence_task, "rec_tx", 3072, nullptr, 4, &s_tx_task, 0);
    xTaskCreatePinnedToCore(rec_task,        "rec_rd", 4096, nullptr, 6, &s_rec_task, 1);

    ESP_LOGI(TAG, "recording -> %s", vfs_path);
    return true;
}

void recorder_stop(void)
{
    if (!s_running && !s_file) return;
    s_running = false;

    // Wait for both tasks to exit before touching shared resources.
    if (s_rec_task) { xSemaphoreTake(s_rec_done, pdMS_TO_TICKS(1000)); s_rec_task = nullptr; }
    if (s_tx_task)  { xSemaphoreTake(s_tx_done,  pdMS_TO_TICKS(1000)); s_tx_task  = nullptr; }

    i2s_stop();
    codec_enable_mic(false);

    if (s_file) {
        fflush(s_file);
        // Patch RIFF + data sizes now that the length is known.
        fseek(s_file, 0, SEEK_SET);
        write_wav_header(s_file, s_data_bytes);
        fflush(s_file);
        fclose(s_file);
        s_file = nullptr;
    }
    ESP_LOGI(TAG, "stopped, %u PCM bytes", (unsigned)s_data_bytes);
}

bool recorder_is_active(void) { return s_running; }

uint32_t recorder_elapsed_ms(void)
{
    if (!s_running) return 0;
    return (uint32_t)((esp_timer_get_time() - s_start_us) / 1000);
}

uint32_t recorder_bytes_written(void) { return s_data_bytes; }
