// recorder.cpp — onboard-mic WAV recorder.
//
// Capture path mirrors the proven ESP32-S3-EPaper-Player design:
//   • I2S0 master TX clocks the ES8311 (MCLK/BCLK/WS). The DAC output is unused
//     while recording, but TX must stay enabled & fed with silence so the
//     shared bit/word clocks keep running for the slave RX.
//   • I2S1 slave RX captures the mic, sharing I2S0's BCLK/WS.
//
// I2S framing note (matches the proven ESP32-S3-EPaper-Player capture path):
// BOTH the master TX and the slave RX run 32-bit stereo slots (64 BCLK per WS).
// The ES8311 is configured for a 16-bit ADC word (reg 0x0A) which it emits
// MSB-first at the start of each 32-bit slot, so the real sample lands in
// bits[31:16] and is recovered with ">>16" — exactly like the reference.
// Reading the RX as 16-bit slots instead chops the stream and injects a
// 0x8000 framing artifact on every other word; that path is wrong, do not use.
// The ADC word appears in ONE channel slot (L or R) depending on board WS
// phase — this is auto-detected per recording by cumulative energy.
// The ES8311 ADC aligns cleanly only at its proven 44.1/48 kHz rates, so we
// CAPTURE at 48 kHz and decimate /3 in software into the 16 kHz/16-bit/mono
// WAV that DashScope ASR expects.

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
static char              s_path[128]  = {0};   // current recording path (for diag sidecar)

// Whole-recording capture stats, filled by rec_task, dumped on stop.
// ch 0 = left 32-bit slot, ch 1 = right 32-bit slot (each ADC word in [31:16]).
static int64_t           s_ch_energy[2]   = {0,0};   // AC (mean-removed) energy
static int64_t           s_ch_sum[2]      = {0,0};   // running sum (for mean)
static int64_t           s_ch_sumsq[2]    = {0,0};   // running sum of squares
static uint32_t          s_ch_ghost[2]    = {0,0};   // exact-0x8000 rail-sample count
static uint32_t          s_ch_samples     = 0;
static int               s_ch_used        = 0;
static uint32_t          s_raw[16]        = {0};   // first I2S frames (raw 32-bit L/R)
static int               s_raw_n          = 0;     // valid uint32 count in s_raw

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
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(REC_CAPTURE_RATE),
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
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(REC_CAPTURE_RATE),
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
    // 32-bit slot to match the I2S0 master framing (64 BCLK per WS). The ES8311
    // emits a 16-bit ADC word at the MSB end, so the sample sits in bits[31:16]
    // and is recovered with ">>16" in rec_task. Reading 16-bit slots here injects
    // a 0x8000 framing artifact, so keep the slot width at 32 bits.
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

// ── Recorder task: mic @48k → ch extract (>>16) → /3 decimate → 16k mono WAV ──
// Each captured frame is a 32-bit stereo pair; the ES8311's 16-bit ADC word sits
// in bits[31:16] of one channel slot (L or R, auto-detected by energy). Samples
// are DC-blocked, averaged in groups of REC_DECIM (48k→16k), scaled by
// REC_SW_GAIN and clamped before being written to the 16 kHz WAV.
static void rec_task(void *)
{
    const int FRAMES = 768;                       // 48k stereo frames per read (16 ms, /3 clean)
    size_t  in_bytes = (size_t)FRAMES * 2 * sizeof(uint32_t);
    uint32_t *stereo = (uint32_t *)heap_caps_malloc(in_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t  *mono   = (int16_t  *)heap_caps_malloc((FRAMES / REC_DECIM + 4) * sizeof(int16_t),
                                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t max_bytes = (uint32_t)REC_SAMPLE_RATE * REC_CHANNELS * (REC_BITS / 8) * REC_MAX_SECONDS;

    int      ch          = 0;        // valid ADC channel (0=L, 1=R), auto-detected
    int64_t  sum[2]      = {0,0};    // running per-channel sum (for AC/mean)
    int64_t  sumsq[2]    = {0,0};    // running per-channel sum of squares
    uint32_t ghost[2]    = {0,0};    // exact-0x8000 rail-sample count per channel
    int64_t  rms_acc[2]  = {0,0};
    uint32_t rms_samples = 0;
    uint32_t last_log_ms = 0;
    uint32_t warmup      = 0;        // frames seen (skip first ~150 ms for ch lock)
    bool     ch_locked   = false;
    int      dec_acc     = 0;        // /REC_DECIM decimation accumulator (48k->16k)
    int      dec_cnt     = 0;
    // One-pole DC blocker (y = x - x_prev + a*y_prev) over the decimated stream.
    int32_t  hp_xprev    = 0;
    int32_t  hp_yprev    = 0;

    if (stereo && mono) {
        while (s_running) {
            size_t got = 0;
            esp_err_t r = i2s_channel_read(s_rx, stereo, in_bytes, &got, pdMS_TO_TICKS(200));
            if (r != ESP_OK || got < 2 * sizeof(uint32_t)) continue;

            int n = (int)(got / (2 * sizeof(uint32_t)));   // captured frames @48k

            // Snapshot the very first raw frames (L/R 32-bit words) for the diag.
            if (s_raw_n == 0) {
                int cnt = n * 2; if (cnt > 16) cnt = 16;
                for (int k = 0; k < cnt; k++) s_raw[k] = stereo[k];
                s_raw_n = cnt;
            }

            // Accumulate per-channel stats on the 16-bit ADC word (bits[31:16]):
            // sum + sum-of-squares (-> AC energy) and exact-0x8000 ghost count.
            for (int i = 0; i < n; i++) {
                int16_t s0 = (int16_t)(stereo[i * 2]     >> 16);
                int16_t s1 = (int16_t)(stereo[i * 2 + 1] >> 16);
                sum[0]   += s0;            sum[1]   += s1;
                sumsq[0] += (int64_t)s0 * s0;  sumsq[1] += (int64_t)s1 * s1;
                if ((uint16_t)s0 == 0x8000) ghost[0]++;
                if ((uint16_t)s1 == 0x8000) ghost[1]++;
                rms_acc[0] += (int64_t)s0 * s0;  rms_acc[1] += (int64_t)s1 * s1;
            }
            s_ch_samples += (uint32_t)n;
            warmup       += (uint32_t)n;

            // Channel selection by AC (mean-removed) energy, rejecting a channel
            // whose samples are mostly the 0x8000 negative rail (framing garbage).
            // Locked after ~150 ms so the rest of the recording is consistent.
            if (!ch_locked) {
                uint32_t ns = s_ch_samples ? s_ch_samples : 1;
                int64_t ac0 = sumsq[0] - (sum[0] / (int64_t)ns) * sum[0];
                int64_t ac1 = sumsq[1] - (sum[1] / (int64_t)ns) * sum[1];
                bool bad0 = (ghost[0] * 4 > s_ch_samples);   // >25% rail samples
                bool bad1 = (ghost[1] * 4 > s_ch_samples);
                if (bad0 && !bad1)      ch = 1;
                else if (bad1 && !bad0) ch = 0;
                else                    ch = (ac1 > ac0) ? 1 : 0;
                if (warmup >= (uint32_t)(REC_CAPTURE_RATE * 150 / 1000)) ch_locked = true;
            }
            s_ch_used      = ch;
            s_ch_sum[0] = sum[0]; s_ch_sum[1] = sum[1];
            s_ch_sumsq[0] = sumsq[0]; s_ch_sumsq[1] = sumsq[1];
            s_ch_ghost[0] = ghost[0]; s_ch_ghost[1] = ghost[1];
            {
                uint32_t ns = s_ch_samples ? s_ch_samples : 1;
                s_ch_energy[0] = sumsq[0] - (sum[0] / (int64_t)ns) * sum[0];
                s_ch_energy[1] = sumsq[1] - (sum[1] / (int64_t)ns) * sum[1];
            }

            // Extract the mic channel, decimate /REC_DECIM (48k->16k) by averaging,
            // DC-block, then apply gain. dec_acc/dec_cnt carry across reads so no
            // samples are dropped at chunk boundaries.
            int outn = 0;
            for (int i = 0; i < n; i++) {
                dec_acc += (int16_t)(stereo[i * 2 + ch] >> 16);
                if (++dec_cnt >= REC_DECIM) {
                    int32_t x = dec_acc / REC_DECIM;
                    // y = x - x_prev + (255/256)*y_prev  -> ~5 Hz high-pass @16k
                    int32_t y = x - hp_xprev + (hp_yprev - (hp_yprev >> 8));
                    hp_xprev = x; hp_yprev = y;
                    int v = y * REC_SW_GAIN;
                    if (v >  32767) v =  32767;
                    if (v < -32768) v = -32768;
                    mono[outn++] = (int16_t)v;
                    dec_acc = 0; dec_cnt = 0;
                }
            }
            rms_samples += n;

            size_t wlen = (size_t)outn * sizeof(int16_t);
            if (s_file && outn) fwrite(mono, 1, wlen, s_file);
            s_data_bytes += wlen;

            // Periodic per-channel capture-level log: distinguishes "codec ADC
            // silent" (both channels 0) from "wrong channel" (one channel hot).
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if (now_ms - last_log_ms >= 500 && rms_samples) {
                int rl = (int)__builtin_sqrt((double)(rms_acc[0] / rms_samples));
                int rr2 = (int)__builtin_sqrt((double)(rms_acc[1] / rms_samples));
                ESP_LOGI(TAG, "mic ch RMS  L=%d R=%d  (using ch %d)", rl, rr2, ch);
                rms_acc[0] = rms_acc[1] = 0;
                rms_samples = 0; last_log_ms = now_ms;
            }

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

    // Remember path + reset capture stats for the on-SD diagnostic sidecar.
    snprintf(s_path, sizeof(s_path), "%s", vfs_path);
    s_ch_energy[0] = s_ch_energy[1] = 0;
    s_ch_sum[0] = s_ch_sum[1] = 0;
    s_ch_sumsq[0] = s_ch_sumsq[1] = 0;
    s_ch_ghost[0] = s_ch_ghost[1] = 0;
    s_ch_samples = 0;
    s_ch_used    = 0;
    s_raw_n      = 0;

    // Codec: 48 kHz capture (proven mic rate), mic on with healthy gain.
    codec_set_sample_rate(REC_CAPTURE_RATE);
    codec_enable_mic(true);
    codec_set_mic_gain(6);
    codec_dac_mute(true);          // speaker silent during capture
    codec_read_all();              // one-shot register dump for capture debugging

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

// Write a human-readable diagnostic next to the WAV so capture problems can be
// diagnosed straight from the exported SD card (no serial console needed).
static void write_diag_sidecar(void)
{
    if (s_path[0] == 0) return;

    // Snapshot the key ES8311 registers while the mic is still enabled.
    struct { uint8_t reg; const char *name; } regs[] = {
        {0x00,"reset/csm"}, {0x01,"clkmgr"}, {0x06,"bclk"},  {0x09,"sdpin"},
        {0x0A,"sdpout"},    {0x0D,"anapwr"}, {0x0E,"pga/adc"},{0x12,"dacpwr"},
        {0x14,"mic_sel"},   {0x15,"adc_eq"}, {0x16,"pga_gain"},{0x17,"adc_vol"},
        {0x31,"dac_mute"},  {0x44,"refsig"},
    };

    char path[160];
    snprintf(path, sizeof(path), "%s.diag.txt", s_path);
    FILE *d = fopen(path, "w");
    if (!d) { ESP_LOGW(TAG, "diag open failed: %s", path); return; }

    uint32_t ns = s_ch_samples ? s_ch_samples : 1;
    int rms[2], mean[2], ghostpct[2];
    for (int c = 0; c < 2; c++) {
        rms[c]      = (int)__builtin_sqrt((double)(s_ch_energy[c] / ns));  // AC RMS
        mean[c]     = (int)(s_ch_sum[c] / (int64_t)ns);
        ghostpct[c] = (int)(s_ch_ghost[c] * 100 / ns);
    }

    fprintf(d, "Glane Notes capture diagnostic\n");
    fprintf(d, "wav            : %s\n", s_path);
    fprintf(d, "pcm_bytes      : %u\n", (unsigned)s_data_bytes);
    fprintf(d, "sample_rate    : %d Hz, %d-bit, %d ch\n",
            REC_SAMPLE_RATE, REC_BITS, REC_CHANNELS);
    fprintf(d, "sw_gain        : %dx\n", REC_SW_GAIN);
    fprintf(d, "capture        : %d Hz, 32-bit slot, ADC word in bits[31:16]\n", REC_CAPTURE_RATE);
    fprintf(d, "ch_used        : %d (%s)\n", s_ch_used, s_ch_used ? "right" : "left");
    fprintf(d, "ch_AC_RMS      : L=%d R=%d   (mean-removed; real audio = high here)\n",
            rms[0], rms[1]);
    fprintf(d, "ch_mean        : L=%d R=%d   (DC bias; near 0 = good)\n", mean[0], mean[1]);
    fprintf(d, "ch_ghost_0x8000: L=%d%% R=%d%%  (rail framing garbage; near 0%% = good)\n",
            ghostpct[0], ghostpct[1]);
    fprintf(d, "interpretation : %s\n",
            (rms[0] | rms[1]) == 0
              ? "BOTH channels zero -> ES8311 ADC delivering digital silence (codec/analog)"
              : (ghostpct[s_ch_used] > 20
                  ? "used channel still has 0x8000 ghosts -> I2S framing not aligned"
                  : "used channel carries clean AC audio -> capture path working"));
    fprintf(d, "\nES8311 registers:\n");
    for (size_t i = 0; i < sizeof(regs)/sizeof(regs[0]); i++)
        fprintf(d, "  0x%02X %-9s = 0x%02X\n",
                regs[i].reg, regs[i].name, codec_get_reg(regs[i].reg));
    fprintf(d, "\nexpected: 0x14=0x5A(mic on) 0x17=0xC8(adc vol) 0x0E=0x02 0x0D=0x01\n");
    fprintf(d, "\nfirst raw I2S frames (L32 R32 hex, ADC sample = word>>16):\n");
    for (int i = 0; i + 2 <= s_raw_n; i += 2) {
        fprintf(d, "  %08X %08X   ->  L=%6d R=%6d\n",
                (unsigned)s_raw[i], (unsigned)s_raw[i+1],
                (int)(int16_t)(s_raw[i] >> 16), (int)(int16_t)(s_raw[i+1] >> 16));
    }
    fclose(d);
    ESP_LOGI(TAG, "diag written: %s (chRMS L=%d R=%d used=%d)",
             path, rms[0], rms[1], s_ch_used);
}

void recorder_stop(void)
{
    if (!s_running && !s_file) return;
    s_running = false;

    // Wait for both tasks to exit before touching shared resources.
    if (s_rec_task) { xSemaphoreTake(s_rec_done, pdMS_TO_TICKS(1000)); s_rec_task = nullptr; }
    if (s_tx_task)  { xSemaphoreTake(s_tx_done,  pdMS_TO_TICKS(1000)); s_tx_task  = nullptr; }

    i2s_stop();
    write_diag_sidecar();          // snapshot regs while mic still enabled
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
