// player.cpp — stream a WAV file from SD to the ES8311 DAC + speaker.
//
// Only ever runs while idle (the recorder/tone also own I2S0, so the app state
// machine guarantees mutual exclusion). A 16-bit PCM mono sample is duplicated
// into both L/R 32-bit Philips slots (DAC reads bits[31:16]). On stop/EOF we
// push a short tail of silence and wait for the DMA queue to drain before
// muting the DAC and switching the amplifier off, to avoid clipping/click.

#include "player.h"
#include "glane_config.h"
#include "codec.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

extern "C" {
#include "driver/i2s_std.h"
#include "driver/i2s_common.h"
}

static const char *TAG = "player";

static i2s_chan_handle_t s_tx = nullptr;
static volatile bool     s_running = false;
static volatile bool     s_stop    = false;
static TaskHandle_t      s_task    = nullptr;
static SemaphoreHandle_t s_done    = nullptr;
static char              s_path[160];

// ── Minimal WAV parser ───────────────────────────────────────────────────────
// Fills rate/bits/channels and seeks `f` to the start of the data chunk,
// returning the data length in bytes (0 on failure).
static uint32_t wav_open(FILE *f, uint32_t *rate, uint16_t *bits, uint16_t *channels)
{
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12) return 0;
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) return 0;

    uint16_t fmt_tag = 1, ch = 1, bps = 16;
    uint32_t sr = 16000;
    bool have_fmt = false;

    for (;;) {
        uint8_t ch8[8];
        if (fread(ch8, 1, 8, f) != 8) return 0;
        uint32_t csize = ch8[4] | (ch8[5] << 8) | (ch8[6] << 16) | ((uint32_t)ch8[7] << 24);

        if (memcmp(ch8, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            uint32_t want = csize < 16 ? csize : 16;
            if (fread(fmt, 1, want, f) != want) return 0;
            fmt_tag = fmt[0] | (fmt[1] << 8);
            ch      = fmt[2] | (fmt[3] << 8);
            sr      = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
            bps     = fmt[14] | (fmt[15] << 8);
            have_fmt = true;
            if (csize > want) fseek(f, csize - want, SEEK_CUR);
            if (csize & 1) fseek(f, 1, SEEK_CUR);   // word alignment
        } else if (memcmp(ch8, "data", 4) == 0) {
            if (!have_fmt || fmt_tag != 1) return 0;   // PCM only
            *rate = sr; *bits = bps; *channels = ch;
            return csize;   // f is now positioned at the PCM data
        } else {
            fseek(f, csize + (csize & 1), SEEK_CUR);   // skip unknown chunk
        }
    }
}

static void audio_on(uint32_t rate)
{
    codec_set_sample_rate(rate);
    codec_set_volume(75);
    codec_dac_mute(false);
    gpio_set_level((gpio_num_t)PA_PIN, 1);   // amp on
    vTaskDelay(pdMS_TO_TICKS(8));
}

static void audio_off(void)
{
    codec_dac_mute(true);
    gpio_set_level((gpio_num_t)PA_PIN, 0);   // amp off
}

static bool i2s_open(uint32_t rate)
{
    i2s_chan_config_t cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cfg.dma_desc_num  = 6;
    cfg.dma_frame_num = 240;
    if (i2s_new_channel(&cfg, &s_tx, nullptr) != ESP_OK) return false;

    i2s_std_config_t std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(rate),
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
    if (i2s_channel_init_std_mode(s_tx, &std) != ESP_OK) { i2s_del_channel(s_tx); s_tx = nullptr; return false; }
    if (i2s_channel_enable(s_tx) != ESP_OK)              { i2s_del_channel(s_tx); s_tx = nullptr; return false; }
    return true;
}

static void i2s_close(void)
{
    if (!s_tx) return;
    i2s_channel_disable(s_tx);
    i2s_del_channel(s_tx);
    s_tx = nullptr;
}

static void play_task(void *)
{
    FILE *f = fopen(s_path, "rb");
    if (!f) { ESP_LOGE(TAG, "open %s failed", s_path); goto finish; }

    {
        uint32_t rate = 16000; uint16_t bits = 16, ch = 1;
        uint32_t remaining = wav_open(f, &rate, &bits, &ch);
        if (remaining == 0 || bits != 16 || ch < 1 || ch > 2) {
            ESP_LOGE(TAG, "unsupported WAV (bits=%u ch=%u)", bits, ch);
            fclose(f); goto finish;
        }
        if (!i2s_open(rate)) { fclose(f); goto finish; }
        audio_on(rate);

        const int FRAMES = 512;
        int16_t *in  = (int16_t *)heap_caps_malloc(FRAMES * ch * sizeof(int16_t),
                                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        int32_t *out = (int32_t *)heap_caps_malloc(FRAMES * 2 * sizeof(int32_t),
                                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (in && out) {
            while (remaining > 0 && !s_stop) {
                size_t want = FRAMES * ch * sizeof(int16_t);
                if (want > remaining) want = remaining;
                size_t got = fread(in, 1, want, f);
                if (got == 0) break;
                int frames = (int)(got / (ch * sizeof(int16_t)));
                for (int i = 0; i < frames; i++) {
                    int16_t s = (ch == 1) ? in[i] : in[i * 2];   // left if stereo
                    int32_t slot = (int32_t)s << 16;
                    out[i * 2 + 0] = slot;
                    out[i * 2 + 1] = slot;
                }
                size_t w = 0;
                i2s_channel_write(s_tx, out, (size_t)frames * 2 * sizeof(int32_t),
                                  &w, pdMS_TO_TICKS(300));
                remaining -= got;
            }
            // Tail of silence so the DMA queue empties cleanly before muting.
            memset(out, 0, FRAMES * 2 * sizeof(int32_t));
            for (int k = 0; k < 4; k++) {
                size_t w = 0;
                i2s_channel_write(s_tx, out, FRAMES * 2 * sizeof(int32_t), &w, pdMS_TO_TICKS(100));
            }
            vTaskDelay(pdMS_TO_TICKS(60));
        }
        heap_caps_free(in);
        heap_caps_free(out);

        audio_off();
        i2s_close();
        fclose(f);
    }

finish:
    s_running = false;
    xSemaphoreGive(s_done);
    s_task = nullptr;
    vTaskDelete(nullptr);
}

// ── Public API ───────────────────────────────────────────────────────────────
void player_init(void)
{
    if (!s_done) s_done = xSemaphoreCreateBinary();
}

bool player_play(const char *wav_path)
{
    if (s_running) return false;
    strncpy(s_path, wav_path, sizeof(s_path) - 1);
    s_path[sizeof(s_path) - 1] = '\0';
    s_stop    = false;
    s_running = true;
    xSemaphoreTake(s_done, 0);
    if (xTaskCreatePinnedToCore(play_task, "player", 4096, nullptr, 5, &s_task, 1) != pdPASS) {
        s_running = false;
        return false;
    }
    return true;
}

void player_stop(void)
{
    if (!s_running) return;
    s_stop = true;
    xSemaphoreTake(s_done, pdMS_TO_TICKS(2000));
}

bool player_is_active(void) { return s_running; }
