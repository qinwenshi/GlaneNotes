// tone.cpp — short UI feedback tones through the ES8311 DAC + speaker.
//
// Glane Notes normally keeps the amplifier (PA_PIN) off and the DAC muted; the
// only audio path used during a recording is the mic ADC. To give the user an
// audible "recording started / stopped" cue we briefly:
//   1. enable the PA and un-mute the DAC,
//   2. stream a short sine-wave melody out of I2S0 master TX (same pins the
//      recorder uses to clock the codec — DOUT 45 carries the DAC samples),
//   3. mute the DAC and switch the PA back off.
//
// These cues are only ever played while idle (never mid-recording), so I2S0 is
// free. Each call sets up and tears down its own I2S channel and blocks until
// the melody has finished playing.

#include "tone.h"
#include "glane_config.h"
#include "codec.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>

extern "C" {
#include "driver/i2s_std.h"
#include "driver/i2s_common.h"
}

static const char *TAG = "tone";

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TONE_SR        REC_SAMPLE_RATE   // 16 kHz, reuse the recorder rate
#define TONE_AMPLITUDE 0.25f             // 0..1 of full scale (keep it gentle)
#define TONE_FADE_MS   4                 // linear in/out ramp to avoid clicks

// Open I2S0 as master TX, identical framing to the recorder's clock source so
// the ES8311 stays happy (32-bit Philips stereo slots, DAC data in bits[31:16]).
static i2s_chan_handle_t tone_i2s_open(void)
{
    i2s_chan_handle_t tx = nullptr;
    i2s_chan_config_t cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cfg.dma_desc_num  = 6;
    cfg.dma_frame_num = 240;
    if (i2s_new_channel(&cfg, &tx, nullptr) != ESP_OK) return nullptr;

    i2s_std_config_t std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(TONE_SR),
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
    if (i2s_channel_init_std_mode(tx, &std) != ESP_OK) { i2s_del_channel(tx); return nullptr; }
    if (i2s_channel_enable(tx) != ESP_OK)              { i2s_del_channel(tx); return nullptr; }
    return tx;
}

static void tone_i2s_close(i2s_chan_handle_t tx)
{
    if (!tx) return;
    i2s_channel_disable(tx);
    i2s_del_channel(tx);
}

// Synthesize `freq_hz` for `ms` into the open TX channel. A freq of 0 emits
// silence (useful as a gap between notes). Blocking.
static void tone_render(i2s_chan_handle_t tx, int freq_hz, int ms, float *phase)
{
    if (ms <= 0) return;
    const int    total   = (TONE_SR * ms) / 1000;
    const int    fade    = (TONE_SR * TONE_FADE_MS) / 1000;
    const float  dphase  = freq_hz > 0 ? (float)(2.0 * M_PI * freq_hz / TONE_SR) : 0.0f;

    const int FRAMES = 256;
    int32_t *buf = (int32_t *)heap_caps_malloc(FRAMES * 2 * sizeof(int32_t),
                                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) return;

    int done = 0;
    while (done < total) {
        int n = total - done; if (n > FRAMES) n = FRAMES;
        for (int i = 0; i < n; i++) {
            int idx = done + i;
            float env = 1.0f;
            if (idx < fade)            env = (float)idx / fade;            // fade in
            else if (idx > total - fade) env = (float)(total - idx) / fade; // fade out
            if (env < 0.0f) env = 0.0f;

            int16_t s = 0;
            if (freq_hz > 0) {
                s = (int16_t)(sinf(*phase) * TONE_AMPLITUDE * env * 32767.0f);
                *phase += dphase;
                if (*phase > (float)(2.0 * M_PI)) *phase -= (float)(2.0 * M_PI);
            }
            int32_t slot = (int32_t)s << 16;   // ES8311 DAC reads bits[31:16]
            buf[i * 2 + 0] = slot;             // left
            buf[i * 2 + 1] = slot;             // right
        }
        size_t w = 0;
        i2s_channel_write(tx, buf, (size_t)n * 2 * sizeof(int32_t), &w, pdMS_TO_TICKS(200));
        done += n;
    }
    heap_caps_free(buf);
}

// Play a melody (note frequencies + durations) with the audio path enabled
// once around the whole sequence to minimise pops.
static void tone_play_sequence(const int *freqs, const int *durs, int n)
{
    i2s_chan_handle_t tx = tone_i2s_open();
    if (!tx) { ESP_LOGW(TAG, "i2s open failed"); return; }

    codec_set_sample_rate(TONE_SR);
    codec_set_volume(70);
    codec_dac_mute(false);
    gpio_set_level((gpio_num_t)PA_PIN, 1);   // amp on (active HIGH)
    vTaskDelay(pdMS_TO_TICKS(8));            // let the amp settle

    float phase = 0.0f;
    for (int i = 0; i < n; i++) tone_render(tx, freqs[i], durs[i], &phase);

    // Flush a few ms of silence so the last samples clock fully out, then hush.
    tone_render(tx, 0, 8, &phase);
    codec_dac_mute(true);
    gpio_set_level((gpio_num_t)PA_PIN, 0);   // amp off
    tone_i2s_close(tx);
}

void tone_beep(int freq_hz, int duration_ms)
{
    int f = freq_hz, d = duration_ms;
    tone_play_sequence(&f, &d, 1);
}

void tone_play_start(void)
{
    // Rising cue: A5 -> E6.
    static const int f[] = { 880, 1318 };
    static const int d[] = { 90, 120 };
    tone_play_sequence(f, d, 2);
}

void tone_play_stop(void)
{
    // Falling cue: E6 -> A5.
    static const int f[] = { 1318, 880 };
    static const int d[] = { 90, 120 };
    tone_play_sequence(f, d, 2);
}
