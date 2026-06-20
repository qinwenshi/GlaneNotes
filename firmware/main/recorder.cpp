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
// The ES8311 is configured for a 16-bit ADC word (reg 0x0A); depending on board
// WS phase that 16-bit word lands in the HIGH (bits[31:16]) or LOW (bits[15:0])
// half of one channel slot (L or R). So per recording we auto-detect among the
// four candidates {L-hi, L-lo, R-hi, R-lo} by AC (mean-removed) energy with
// 0x8000 rail-ghost rejection — on this board the data sits in the LOW half.
// Reading the RX as 16-bit slots instead chops the stream and injects a
// 0x8000 framing artifact on every other word; that path is wrong, do not use.
// The ES8311 ADC aligns cleanly only at its proven 44.1/48 kHz rates, so we
// CAPTURE at 48 kHz and decimate /3 in software into the 16 kHz/16-bit/mono
// WAV that DashScope ASR expects.

#include "recorder.h"
#include "glane_config.h"
#include "codec.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"
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
static volatile uint32_t s_data_bytes = 0;        // PCM bytes actually written to SD
static volatile uint32_t s_prod_bytes = 0;        // PCM bytes produced by capture loop
static int64_t           s_start_us   = 0;
static char              s_path[128]  = {0};   // current recording path (for diag sidecar)

// Decoupled SD writer: the capture loop pushes decimated 16k samples into this
// PSRAM ring buffer and a separate task drains it to the SD card, so a blocking
// SD write never stalls the I2S reader (which would overflow the RX DMA and drop
// samples — the cause of the "too fast / voice-changer" playback).
static RingbufHandle_t   s_ring       = nullptr;
static TaskHandle_t      s_write_task = nullptr;
static SemaphoreHandle_t s_write_done = nullptr;
static volatile bool     s_rec_finished = false;     // producer fully done enqueuing
static volatile uint32_t s_ring_drops = 0;        // bytes dropped on ring-full (should stay 0)

// Whole-recording capture stats, filled by rec_task, dumped on stop.
// Four candidate sources per 32-bit stereo frame: the ADC's 16-bit word can sit
// in the high or low half of either channel slot depending on board alignment.
//   cand 0 = L[31:16]  1 = L[15:0]  2 = R[31:16]  3 = R[15:0]
static int64_t           s_cand_energy[4] = {0,0,0,0};  // AC (mean-removed) energy
static int64_t           s_cand_sum[4]    = {0,0,0,0};  // running sum (for mean)
static int64_t           s_cand_sumsq[4]  = {0,0,0,0};  // running sum of squares
static uint32_t          s_cand_ghost[4]  = {0,0,0,0};  // exact-0x8000 rail count
static uint32_t          s_cand_samples   = 0;
static int               s_cand_used      = 0;
static uint32_t          s_raw[16]        = {0};   // first I2S frames (raw 32-bit L/R)
static int               s_raw_n          = 0;     // valid uint32 count in s_raw

// Extract candidate c (0..3) from a frame's left/right 32-bit slot words.
static inline int16_t extract_cand(uint32_t lw, uint32_t rw, int c)
{
    uint32_t w = (c < 2) ? lw : rw;
    return (c & 1) ? (int16_t)(w & 0xFFFF) : (int16_t)(w >> 16);
}

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
    // 16-bit ADC word lands in the high or low half of a channel slot (board
    // dependent), recovered by the auto-detected candidate in rec_task. Reading
    // 16-bit slots here injects a 0x8000 framing artifact, so keep it 32 bits.
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

// Delay in small slices so a recorder_stop() during startup priming is honored
// promptly (bounds the abort latency, keeping recorder_stop()'s join wait safe).
// Returns false if recording was stopped while waiting.
static bool running_delay(uint32_t ms)
{
    for (uint32_t e = 0; e < ms && s_running; e += 20)
        vTaskDelay(pdMS_TO_TICKS(20));
    return s_running;
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

// ── 48 kHz -> 16 kHz anti-aliased 3:1 decimator ──────────────────────────────
// A symmetric 23-tap low-pass FIR (Hamming window, ~7 kHz cutoff, unity DC gain,
// Q15) applied to the 48 kHz candidate stream; one output is emitted every third
// input sample. Unlike the boxcar /3 average (weak stopband -> hiss aliases into
// band) or a fractional resampler (which, at the exact 3:1 ratio, alternates
// single-/double-tap filtering and imposes an 8 kHz modulation that sounds like a
// voice-changer), this treats every output identically: correct pitch, clean
// stopband, no modulation. Filter state (hist/phase) persists across reads.
#define DECIM_NTAPS 23
static const int16_t DECIM_COEF[DECIM_NTAPS] = {
    -46, 27, 164, 270, 68, -583, -1252, -953, 1119, 4670, 8122, 9556,
    8122, 4670, 1119, -953, -1252, -583, 68, 270, 164, 27, -46
};

// ── Recorder task: mic @48k → candidate extract → FIR decimate 16k → WAV ──────
// Each captured frame is a 32-bit stereo pair. The ES8311's 16-bit ADC word may
// land in the high OR low half of either channel slot (board-dependent), so we
// auto-detect among four candidates (L-hi, L-lo, R-hi, R-lo) by AC (mean-removed)
// energy with 0x8000 rail-ghost rejection, lock the choice after ~150 ms, then
// 3:1 FIR-decimate 48k→16k (anti-aliased), DC-block, apply gain, clamp into WAV.
static void rec_task(void *)
{
    const int FRAMES = 768;                       // 48k stereo frames per read (16 ms)
    size_t  in_bytes = (size_t)FRAMES * 2 * sizeof(uint32_t);
    uint32_t *stereo = (uint32_t *)heap_caps_malloc(in_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t  *mono   = (int16_t *)heap_caps_malloc((FRAMES / 3 + 8) * sizeof(int16_t),
                                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t max_bytes = (uint32_t)REC_SAMPLE_RATE * REC_CHANNELS * (REC_BITS / 8) * REC_MAX_SECONDS;

    int      sel         = 1;        // chosen candidate (0..3), default L-low
    int64_t  sum[4]      = {0,0,0,0};
    int64_t  sumsq[4]    = {0,0,0,0};
    uint32_t ghost[4]    = {0,0,0,0};
    uint32_t last_log_ms = 0;
    uint32_t warmup      = 0;        // frames seen (skip first ~150 ms for sel lock)
    bool     sel_locked  = false;
    int16_t  hist[DECIM_NTAPS] = {0}; // FIR delay line (hist[0] = newest), 48k
    int      phase       = 0;         // 3:1 decimation phase (emit every 3rd input)
    // One-pole DC blocker (y = x - x_prev + a*y_prev) over the 16k stream.
    int32_t  hp_xprev    = 0;
    int32_t  hp_yprev    = 0;
    bool     hp_init     = false;

    if (stereo && mono) {
        // ── Startup priming: settle analog, flush stale DMA, recover dead ADC ──
        // The shared I2S0 clocks are already live (tx_silence_task), so the
        // ES8311 analog path settles during this delay. Then flush any startup
        // garbage from the RX DMA before measuring. If the mic reads as digital
        // silence (intermittent 0x0D=0x02 dead-ADC state), re-assert codec power
        // and bounce the I2S RX channel, then retry. This runs before any audio
        // is written; the steady loop below never recovers mid-recording.
        running_delay(REC_SETTLE_MS);
        for (int attempt = 0; s_running && attempt <= REC_PRIME_RETRIES; attempt++) {
            // Drain stale DMA frames (startup garbage / 0xFFFF rail) first.
            for (int f = 0; f < 4 && s_running; f++) {
                size_t g = 0;
                i2s_channel_read(s_rx, stereo, in_bytes, &g, pdMS_TO_TICKS(100));
            }
            // Probe ~150 ms; measure max per-candidate AC (mean-removed) energy.
            int64_t  psum[4] = {0,0,0,0}, psumsq[4] = {0,0,0,0};
            uint32_t pn = 0;
            const uint32_t target = (uint32_t)(REC_CAPTURE_RATE * 150 / 1000);
            while (s_running && pn < target) {
                size_t g = 0;
                if (i2s_channel_read(s_rx, stereo, in_bytes, &g, pdMS_TO_TICKS(200)) != ESP_OK)
                    continue;
                int m = (int)(g / (2 * sizeof(uint32_t)));
                for (int i = 0; i < m; i++) {
                    uint32_t lw = stereo[i * 2], rw = stereo[i * 2 + 1];
                    for (int c = 0; c < 4; c++) {
                        int16_t s = extract_cand(lw, rw, c);
                        psum[c]   += s;
                        psumsq[c] += (int64_t)s * s;
                    }
                }
                pn += (uint32_t)m;
            }
            if (!s_running) break;
            uint32_t pns = pn ? pn : 1;
            int64_t maxac = 0;
            for (int c = 0; c < 4; c++) {
                int64_t ac = psumsq[c] - (psum[c] / (int64_t)pns) * psum[c];
                if (ac > maxac) maxac = ac;
            }
            int maxrms = (int)__builtin_sqrt((double)(maxac / pns));
            if (maxrms >= REC_SILENCE_FLOOR) break;     // live mic -> proceed
            if (attempt == REC_PRIME_RETRIES) {
                ESP_LOGW(TAG, "mic still silent after %d recovery attempts; recording anyway",
                         attempt);
                break;
            }
            ESP_LOGW(TAG, "mic digital-silent (max AC RMS=%d) -> codec reset + I2S resync (attempt %d)",
                     maxrms, attempt + 1);
            codec_reset();                              // full CSM reset re-runs analog ramp
            codec_set_sample_rate(REC_CAPTURE_RATE);
            codec_enable_mic(true);
            codec_set_mic_gain(REC_MIC_PGA_GAIN);
            codec_dac_mute(true);
            i2s_channel_disable(s_rx);
            running_delay(30);
            i2s_channel_enable(s_rx);
            running_delay(200);                         // let DMA refill with live data
        }
        // Diag raw snapshot should show the real recording, not flushed garbage.
        s_raw_n = 0;

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

            // Accumulate per-candidate stats: sum + sum-of-squares (-> AC energy)
            // and exact-0x8000 ghost count, over all four high/low × L/R sources.
            for (int i = 0; i < n; i++) {
                uint32_t lw = stereo[i * 2], rw = stereo[i * 2 + 1];
                for (int c = 0; c < 4; c++) {
                    int16_t s = extract_cand(lw, rw, c);
                    sum[c]   += s;
                    sumsq[c] += (int64_t)s * s;
                    if ((uint16_t)s == 0x8000) ghost[c]++;
                }
            }
            s_cand_samples += (uint32_t)n;
            warmup         += (uint32_t)n;

            // Candidate selection by AC (mean-removed) energy, rejecting any
            // candidate that is mostly the 0x8000 rail (framing garbage). Locked
            // after ~150 ms so the rest of the recording is consistent.
            uint32_t ns = s_cand_samples ? s_cand_samples : 1;
            int64_t ac[4];
            for (int c = 0; c < 4; c++) {
                ac[c] = sumsq[c] - (sum[c] / (int64_t)ns) * sum[c];
                s_cand_energy[c] = ac[c];
                s_cand_sum[c]    = sum[c];
                s_cand_sumsq[c]  = sumsq[c];
                s_cand_ghost[c]  = ghost[c];
            }
            if (!sel_locked) {
                int best = -1; int64_t bestac = -1;
                for (int c = 0; c < 4; c++) {
                    if (ghost[c] * 4 > s_cand_samples) continue;   // >25% rail -> reject
                    if (ac[c] > bestac) { bestac = ac[c]; best = c; }
                }
                if (best >= 0) sel = best;
                if (warmup >= (uint32_t)(REC_CAPTURE_RATE * 150 / 1000)) sel_locked = true;
            }
            s_cand_used = sel;

            // Extract the chosen candidate at 48 kHz, push through the FIR delay
            // line, and emit one anti-aliased 16 kHz output every third sample;
            // then DC-block and apply gain. Output is gated until the candidate
            // locks, dropping the loud ADC start-up transient.
            int outn = 0;
            for (int i = 0; i < n; i++) {
                int16_t s = extract_cand(stereo[i * 2], stereo[i * 2 + 1], sel);
                for (int k = DECIM_NTAPS - 1; k > 0; k--) hist[k] = hist[k - 1];
                hist[0] = s;
                if (++phase >= 3) {
                    phase = 0;
                    int32_t acc = 0;
                    for (int k = 0; k < DECIM_NTAPS; k++)
                        acc += (int32_t)DECIM_COEF[k] * hist[k];
                    int32_t x = acc >> 15;          // decimated 16k sample
                    if (!hp_init) { hp_xprev = x; hp_yprev = 0; hp_init = true; }
                    // y = x - x_prev + (255/256)*y_prev  -> ~5 Hz high-pass @16k
                    int32_t y = x - hp_xprev + (hp_yprev - (hp_yprev >> 8));
                    hp_xprev = x; hp_yprev = y;
                    if (sel_locked) {               // skip warmup transient
                        int v = y * REC_SW_GAIN;
                        if (v >  32767) v =  32767;
                        if (v < -32768) v = -32768;
                        mono[outn++] = (int16_t)v;
                    }
                }
            }

            size_t wlen = (size_t)outn * sizeof(int16_t);
            if (s_ring && outn) {
                // Push to the SD-writer ring instead of writing here. A short
                // timeout only bites if the writer can't keep up (won't, at
                // ~32 KB/s vs MB/s SD); dropping rather than blocking keeps the
                // reader tight so the I2S RX DMA never overflows — preserving the
                // true sample rate (a stalled reader drops samples and the clip
                // plays back too fast, the "voice-changer" artifact).
                if (xRingbufferSend(s_ring, mono, wlen, pdMS_TO_TICKS(8)) != pdTRUE) {
                    s_ring_drops += (uint32_t)wlen;
                } else {
                    s_prod_bytes += (uint32_t)wlen;
                }
            }

            // Periodic capture-level log (AC RMS of the chosen candidate).
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if (now_ms - last_log_ms >= 500) {
                int acrms = (int)__builtin_sqrt((double)(ac[sel] / ns));
                ESP_LOGI(TAG, "mic AC RMS cand[%d]=%d  (Lhi=%lld Llo=%lld Rhi=%lld Rlo=%lld)",
                         sel, acrms, (long long)ac[0], (long long)ac[1],
                         (long long)ac[2], (long long)ac[3]);
                last_log_ms = now_ms;
            }

            if (s_prod_bytes >= max_bytes) {
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
    s_rec_finished = true;          // signal writer: no more items will be enqueued
    xSemaphoreGive(s_rec_done);
    vTaskDelete(nullptr);
}

// ── SD writer task ───────────────────────────────────────────────────────────
// Drains the capture ring buffer to the SD card. Runs decoupled from rec_task so
// a blocking SD write never stalls the I2S reader. Exits only once the producer
// has finished (s_rec_finished) AND the ring is fully drained, so no captured
// audio is lost on stop.
static void write_task(void *)
{
    for (;;) {
        size_t len = 0;
        void *item = xRingbufferReceive(s_ring, &len, pdMS_TO_TICKS(100));
        if (item) {
            if (s_file && len) {
                size_t n = fwrite(item, 1, len, s_file);
                s_data_bytes += (uint32_t)n;
                if (n != len) ESP_LOGW(TAG, "SD short write %u/%u", (unsigned)n, (unsigned)len);
            }
            vRingbufferReturnItem(s_ring, item);
            continue;   // drain greedily while data is available
        }
        // NULL receive (ring empty for 100 ms). Exit only after the producer has
        // signalled it is fully done — using s_running alone would race the
        // reader's final in-flight chunk and drop tail audio.
        if (s_rec_finished) break;
    }
    xSemaphoreGive(s_write_done);
    vTaskDelete(nullptr);
}

// ── Public API ───────────────────────────────────────────────────────────────
void recorder_init(void)
{
    if (!s_rec_done)   s_rec_done   = xSemaphoreCreateBinary();
    if (!s_tx_done)    s_tx_done    = xSemaphoreCreateBinary();
    if (!s_write_done) s_write_done = xSemaphoreCreateBinary();
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
    for (int c = 0; c < 4; c++) {
        s_cand_energy[c] = s_cand_sum[c] = s_cand_sumsq[c] = 0;
        s_cand_ghost[c]  = 0;
    }
    s_cand_samples = 0;
    s_cand_used    = 1;
    s_raw_n        = 0;

    // Create the PSRAM ring buffer + spin up the SD writer task (decoupled from
    // capture). NO-SPLIT so each push is returned as one contiguous block.
    s_ring = xRingbufferCreateWithCaps(REC_RING_BYTES, RINGBUF_TYPE_BYTEBUF,
                                       MALLOC_CAP_SPIRAM);
    if (!s_ring) {
        ESP_LOGE(TAG, "ring buffer alloc failed");
        fclose(s_file);
        s_file = nullptr;
        return false;
    }
    s_prod_bytes = 0;
    s_ring_drops = 0;
    s_rec_finished = false;

    // Codec: full power-up sequence first. Re-poking power registers does not
    // re-trigger the ES8311 analog state machine, so a codec left in a stuck
    // silent-ADC state (identical register values yet no analog signal, seen
    // after deep-sleep wake or playback) is only cleared by re-running the soft
    // reset. Then configure for 48 kHz capture (proven mic rate) with mic on.
    codec_reset();
    codec_set_sample_rate(REC_CAPTURE_RATE);
    codec_enable_mic(true);
    codec_set_mic_gain(REC_MIC_PGA_GAIN);
    codec_dac_mute(true);          // speaker silent during capture
    codec_read_all();              // one-shot register dump for capture debugging

    if (!i2s_start()) {
        ESP_LOGE(TAG, "i2s_start failed");
        i2s_stop();
        vRingbufferDeleteWithCaps(s_ring);
        s_ring = nullptr;
        fclose(s_file);
        s_file = nullptr;
        return false;
    }

    s_data_bytes = 0;
    s_start_us   = esp_timer_get_time();
    s_running    = true;

    xSemaphoreTake(s_tx_done, 0);
    xSemaphoreTake(s_rec_done, 0);
    xSemaphoreTake(s_write_done, 0);
    // Writer on core 0 (with TX silence); tight reader alone on core 1.
    BaseType_t ok = pdPASS;
    ok &= xTaskCreatePinnedToCore(write_task,      "rec_wr", 4096, nullptr, 5, &s_write_task, 0);
    ok &= xTaskCreatePinnedToCore(tx_silence_task, "rec_tx", 3072, nullptr, 4, &s_tx_task, 0);
    ok &= xTaskCreatePinnedToCore(rec_task,        "rec_rd", 4096, nullptr, 6, &s_rec_task, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "task create failed — aborting recording");
        s_running = false;
        // Let any started tasks observe s_running==false and exit, then reclaim.
        if (s_rec_task)   { xSemaphoreTake(s_rec_done,   pdMS_TO_TICKS(2000)); s_rec_task   = nullptr; }
        if (s_tx_task)    { xSemaphoreTake(s_tx_done,    pdMS_TO_TICKS(1000)); s_tx_task    = nullptr; }
        s_rec_finished = true;   // unblock writer's exit condition
        if (s_write_task) { xSemaphoreTake(s_write_done, pdMS_TO_TICKS(2000)); s_write_task = nullptr; }
        i2s_stop();
        vRingbufferDeleteWithCaps(s_ring);
        s_ring = nullptr;
        fclose(s_file);
        s_file = nullptr;
        return false;
    }

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

    uint32_t ns = s_cand_samples ? s_cand_samples : 1;
    const char *cname[4] = { "L_hi", "L_lo", "R_hi", "R_lo" };
    int rms[4], mean[4], ghostpct[4];
    for (int c = 0; c < 4; c++) {
        rms[c]      = (int)__builtin_sqrt((double)(s_cand_energy[c] / ns));  // AC RMS
        mean[c]     = (int)(s_cand_sum[c] / (int64_t)ns);
        ghostpct[c] = (int)(s_cand_ghost[c] * 100 / ns);
    }
    int u = s_cand_used;

    fprintf(d, "Glane Notes capture diagnostic\n");
    fprintf(d, "wav            : %s\n", s_path);
    fprintf(d, "pcm_bytes      : %u\n", (unsigned)s_data_bytes);
    fprintf(d, "sample_rate    : %d Hz, %d-bit, %d ch\n",
            REC_SAMPLE_RATE, REC_BITS, REC_CHANNELS);
    // Wall-clock vs audio duration: must match (ratio ~1.00). If audio is much
    // shorter than wall-clock, samples were dropped during capture and the clip
    // plays back too fast (the "voice-changer" artifact).
    {
        double wall_s  = (double)(esp_timer_get_time() - s_start_us) / 1e6;
        double audio_s = (double)s_data_bytes /
                         ((double)REC_SAMPLE_RATE * REC_CHANNELS * (REC_BITS / 8));
        fprintf(d, "timing         : wall=%.2fs audio=%.2fs ratio=%.2f (1.00=no drops)\n",
                wall_s, audio_s, audio_s > 0 ? wall_s / audio_s : 0.0);
        fprintf(d, "ring_drops     : %u bytes produced=%u written=%u\n",
                (unsigned)s_ring_drops, (unsigned)s_prod_bytes, (unsigned)s_data_bytes);
    }
    fprintf(d, "sw_gain        : %dx\n", REC_SW_GAIN);
    fprintf(d, "capture        : %d Hz, 32-bit slot, anti-aliased resample ->%d Hz, DC-blocked\n",
            REC_CAPTURE_RATE, REC_SAMPLE_RATE);
    fprintf(d, "cand_used      : %d (%s)\n", u, cname[u]);
    fprintf(d, "cand_AC_RMS    : Lhi=%d Llo=%d Rhi=%d Rlo=%d  (real audio = high)\n",
            rms[0], rms[1], rms[2], rms[3]);
    fprintf(d, "cand_mean      : Lhi=%d Llo=%d Rhi=%d Rlo=%d  (DC bias)\n",
            mean[0], mean[1], mean[2], mean[3]);
    fprintf(d, "cand_ghost0x8000: Lhi=%d%% Llo=%d%% Rhi=%d%% Rlo=%d%%  (rail garbage; 0%% = good)\n",
            ghostpct[0], ghostpct[1], ghostpct[2], ghostpct[3]);
    fprintf(d, "interpretation : %s\n",
            (rms[0] | rms[1] | rms[2] | rms[3]) == 0
              ? "ALL candidates zero -> ES8311 ADC delivering digital silence (codec/analog)"
              : (rms[u] < 200
                  ? "chosen candidate AC RMS very low -> mic gain or analog path weak"
                  : "chosen candidate carries clean AC audio -> capture path working"));
    fprintf(d, "\nES8311 registers:\n");
    for (size_t i = 0; i < sizeof(regs)/sizeof(regs[0]); i++)
        fprintf(d, "  0x%02X %-9s = 0x%02X\n",
                regs[i].reg, regs[i].name, codec_get_reg(regs[i].reg));
    fprintf(d, "\nexpected: 0x14=0x5A(mic on) 0x17=0xC8(adc vol) 0x0E=0x02 0x0D=0x01\n");
    fprintf(d, "\nfirst raw I2S frames (L32 R32 hex -> Lhi Llo Rhi Rlo int16):\n");
    for (int i = 0; i + 2 <= s_raw_n; i += 2) {
        fprintf(d, "  %08X %08X  -> %6d %6d %6d %6d\n",
                (unsigned)s_raw[i], (unsigned)s_raw[i+1],
                (int)(int16_t)(s_raw[i] >> 16),   (int)(int16_t)(s_raw[i] & 0xFFFF),
                (int)(int16_t)(s_raw[i+1] >> 16), (int)(int16_t)(s_raw[i+1] & 0xFFFF));
    }
    fclose(d);
    ESP_LOGI(TAG, "diag written: %s (cand_used=%d AC RMS=%d)", path, u, rms[u]);
}

void recorder_stop(void)
{
    if (!s_running && !s_file) return;
    s_running = false;

    // Wait for both tasks to exit before touching shared resources. The read
    // task may be mid startup-priming (settle/probe/resync); its delays are
    // sliced so it observes s_running==false within ~200 ms, but allow margin.
    if (s_rec_task) { xSemaphoreTake(s_rec_done, pdMS_TO_TICKS(2000)); s_rec_task = nullptr; }
    if (s_tx_task)  { xSemaphoreTake(s_tx_done,  pdMS_TO_TICKS(1000)); s_tx_task  = nullptr; }

    // Reader has stopped producing (s_rec_finished is set); wait for the writer to
    // flush every remaining ring item to SD before freeing the ring / closing the
    // file. 256 KB drains in well under a second even with SD stalls, so 8 s is
    // ample. If the writer does NOT join (catastrophic SD hang mid-fwrite), it may
    // still be touching s_ring/s_file — do not free them in that case.
    bool writer_joined = true;
    if (s_write_task) {
        writer_joined = (xSemaphoreTake(s_write_done, pdMS_TO_TICKS(8000)) == pdTRUE);
        if (writer_joined) s_write_task = nullptr;
        else ESP_LOGE(TAG, "SD writer did not exit — leaving ring/file to avoid UAF");
    }
    if (writer_joined && s_ring) { vRingbufferDeleteWithCaps(s_ring); s_ring = nullptr; }

    if (s_ring_drops) ESP_LOGW(TAG, "ring dropped %u bytes (writer fell behind)",
                               (unsigned)s_ring_drops);

    i2s_stop();
    write_diag_sidecar();          // snapshot regs while mic still enabled
    codec_enable_mic(false);

    if (writer_joined && s_file) {
        fflush(s_file);
        // Patch RIFF + data sizes now that the length is known.
        fseek(s_file, 0, SEEK_SET);
        write_wav_header(s_file, s_data_bytes);
        fflush(s_file);
        fclose(s_file);
        s_file = nullptr;
    }
    ESP_LOGI(TAG, "stopped, %u PCM bytes (produced %u, dropped %u)",
             (unsigned)s_data_bytes, (unsigned)s_prod_bytes, (unsigned)s_ring_drops);
}

bool recorder_is_active(void) { return s_running; }

uint32_t recorder_elapsed_ms(void)
{
    if (!s_running) return 0;
    return (uint32_t)((esp_timer_get_time() - s_start_us) / 1000);
}

uint32_t recorder_bytes_written(void) { return s_data_bytes; }
