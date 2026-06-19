// transcribe.cpp — file-based ASR using Aliyun DashScope qwen3-asr-flash.
//
// Architecture (per Glane Notes design: "record file, sync file, get text"):
//   • Audio lives on the SD card. Nothing is streamed.
//   • When the device has Wi-Fi and the user triggers a sync, each note's WAV
//     is uploaded in a single HTTPS POST with the audio inlined as a base64
//     data URI (so no public file URL is required — works on a LAN device).
//   • The recognized text comes back in the JSON response and is written to a
//     sibling .txt file on the SD card.
//
// Endpoint: DashScope OpenAI-compatible chat/completions (qwen3-asr-flash).
//   POST https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions
//   Authorization: Bearer <DASHSCOPE_API_KEY>
//   body: { model, messages[ {role:user, content:[
//             {type:"input_audio", input_audio:{data:"data:audio/wav;base64,..."}} ]} ],
//           stream:false, asr_options{ enable_itn } }
//   reply: choices[0].message.content  (a plain UTF-8 string)
// Base64 data-URL audio input is officially supported (<=10 MB file).

#include "transcribe.h"
#include "settings.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

static const char *TAG = "transcribe";

#define DS_URL "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions"
#define DS_MODEL "qwen3-asr-flash"

// Inline-upload cap. base64 inflates by ~4/3, plus JSON overhead. 4 MB of
// base64 ≈ 3 MB WAV ≈ 95 s at 16 kHz mono. Longer notes are skipped (the audio
// is still kept locally; only the auto-transcript is deferred).
#define MAX_WAV_BYTES   (3 * 1024 * 1024)

// ── base64 ───────────────────────────────────────────────────────────────────
static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Encode `len` bytes from src into dst (must hold 4*ceil(len/3)+1). Returns out len.
static size_t base64_encode(const uint8_t *src, size_t len, char *dst)
{
    size_t o = 0;
    size_t i = 0;
    while (i + 3 <= len) {
        uint32_t n = (src[i] << 16) | (src[i + 1] << 8) | src[i + 2];
        dst[o++] = B64[(n >> 18) & 63];
        dst[o++] = B64[(n >> 12) & 63];
        dst[o++] = B64[(n >> 6) & 63];
        dst[o++] = B64[n & 63];
        i += 3;
    }
    size_t rem = len - i;
    if (rem == 1) {
        uint32_t n = src[i] << 16;
        dst[o++] = B64[(n >> 18) & 63];
        dst[o++] = B64[(n >> 12) & 63];
        dst[o++] = '=';
        dst[o++] = '=';
    } else if (rem == 2) {
        uint32_t n = (src[i] << 16) | (src[i + 1] << 8);
        dst[o++] = B64[(n >> 18) & 63];
        dst[o++] = B64[(n >> 12) & 63];
        dst[o++] = B64[(n >> 6) & 63];
        dst[o++] = '=';
    }
    dst[o] = '\0';
    return o;
}

// ── Response accumulation ────────────────────────────────────────────────────
struct resp_ctx {
    char  *buf;
    size_t len;
    size_t cap;
};

static esp_err_t http_evt(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA) {
        struct resp_ctx *r = (struct resp_ctx *)e->user_data;
        if (!r) return ESP_OK;
        if (r->len + e->data_len + 1 > r->cap) {
            size_t ncap = (r->len + e->data_len + 1) * 2;
            char *nb = (char *)heap_caps_realloc(r->buf, ncap, MALLOC_CAP_SPIRAM);
            if (!nb) return ESP_FAIL;
            r->buf = nb;
            r->cap = ncap;
        }
        memcpy(r->buf + r->len, e->data, e->data_len);
        r->len += e->data_len;
        r->buf[r->len] = '\0';
    }
    return ESP_OK;
}

// ── Parse DashScope (OpenAI-compatible) reply → extracted UTF-8 text ─────────
// choices[0].message.content is normally a plain string. Tolerate the array
// form ([{type,text}]) just in case.
static char *parse_text(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return nullptr;
    char *result = nullptr;

    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (cJSON_IsArray(choices)) {
        cJSON *c0 = cJSON_GetArrayItem(choices, 0);
        cJSON *msg = c0 ? cJSON_GetObjectItem(c0, "message") : nullptr;
        cJSON *content = msg ? cJSON_GetObjectItem(msg, "content") : nullptr;
        if (cJSON_IsString(content)) {
            result = strdup(content->valuestring);
        } else if (cJSON_IsArray(content)) {
            size_t total = 0;
            int n = cJSON_GetArraySize(content);
            for (int i = 0; i < n; i++) {
                cJSON *t = cJSON_GetObjectItem(cJSON_GetArrayItem(content, i), "text");
                if (cJSON_IsString(t)) total += strlen(t->valuestring);
            }
            result = (char *)malloc(total + 1);
            if (result) {
                result[0] = '\0';
                for (int i = 0; i < n; i++) {
                    cJSON *t = cJSON_GetObjectItem(cJSON_GetArrayItem(content, i), "text");
                    if (cJSON_IsString(t)) strcat(result, t->valuestring);
                }
            }
        }
    }

    if (!result) {
        // Surface API error messages for debugging.
        cJSON *err = cJSON_GetObjectItem(root, "error");
        cJSON *m = err ? cJSON_GetObjectItem(err, "message")
                       : cJSON_GetObjectItem(root, "message");
        if (cJSON_IsString(m)) ESP_LOGE(TAG, "API error: %s", m->valuestring);
    }
    cJSON_Delete(root);
    return result;
}

// ── Public ───────────────────────────────────────────────────────────────────
tr_result_t transcribe_wav_file(const char *wav_path, char **out_text)
{
    *out_text = nullptr;
    if (!settings_has_api_key()) return TR_NO_KEY;

    struct stat st;
    if (stat(wav_path, &st) != 0) return TR_OPEN_FAIL;
    long fsize = st.st_size;
    if (fsize <= 44) return TR_OPEN_FAIL;
    if (fsize > MAX_WAV_BYTES) {
        ESP_LOGW(TAG, "%s too large (%ld bytes) for inline upload", wav_path, fsize);
        return TR_TOO_LARGE;
    }

    // Read whole WAV (header + PCM) into PSRAM.
    uint8_t *wav = (uint8_t *)heap_caps_malloc(fsize, MALLOC_CAP_SPIRAM);
    if (!wav) return TR_OOM;
    FILE *f = fopen(wav_path, "rb");
    if (!f) { heap_caps_free(wav); return TR_OPEN_FAIL; }
    size_t rd = fread(wav, 1, fsize, f);
    fclose(f);
    if (rd != (size_t)fsize) { heap_caps_free(wav); return TR_OPEN_FAIL; }

    // base64-encode the entire WAV file (DashScope accepts a full wav container).
    size_t b64cap = ((size_t)fsize + 2) / 3 * 4 + 1;
    char *b64 = (char *)heap_caps_malloc(b64cap, MALLOC_CAP_SPIRAM);
    if (!b64) { heap_caps_free(wav); return TR_OOM; }
    base64_encode(wav, fsize, b64);
    heap_caps_free(wav);

    // Build the JSON request body (OpenAI-compatible). Inject the large base64
    // audio data URI by hand to avoid a second copy of the string.
    const char *prefix =
        "{\"model\":\"" DS_MODEL "\","
        "\"messages\":[{\"role\":\"user\",\"content\":["
        "{\"type\":\"input_audio\",\"input_audio\":{\"data\":\"data:audio/wav;base64,";
    const char *suffix =
        "\"}}]}],"
        "\"stream\":false,"
        "\"asr_options\":{\"enable_itn\":true}}";

    size_t blen = strlen(prefix) + strlen(b64) + strlen(suffix) + 1;
    char *body = (char *)heap_caps_malloc(blen, MALLOC_CAP_SPIRAM);
    if (!body) { heap_caps_free(b64); return TR_OOM; }
    strcpy(body, prefix);
    strcat(body, b64);
    strcat(body, suffix);
    heap_caps_free(b64);

    // Response accumulator.
    struct resp_ctx rc = {};
    rc.cap = 4096;
    rc.buf = (char *)heap_caps_malloc(rc.cap, MALLOC_CAP_SPIRAM);
    if (!rc.buf) { heap_caps_free(body); return TR_OOM; }
    rc.buf[0] = '\0';

    esp_http_client_config_t cfg = {};
    cfg.url            = DS_URL;
    cfg.method         = HTTP_METHOD_POST;
    cfg.event_handler  = http_evt;
    cfg.user_data      = &rc;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms     = 60000;
    cfg.buffer_size    = 2048;
    cfg.buffer_size_tx = 2048;

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) { heap_caps_free(body); heap_caps_free(rc.buf); return TR_HTTP_FAIL; }

    char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %s", settings_get()->ds_api_key);
    esp_http_client_set_header(cli, "Authorization", auth);
    esp_http_client_set_header(cli, "Content-Type", "application/json");
    esp_http_client_set_post_field(cli, body, strlen(body));

    ESP_LOGI(TAG, "POST %s (%u byte body)", wav_path, (unsigned)strlen(body));
    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);
    heap_caps_free(body);

    tr_result_t out = TR_OK;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP perform failed: %s", esp_err_to_name(err));
        out = TR_HTTP_FAIL;
    } else if (status != 200) {
        ESP_LOGE(TAG, "HTTP %d: %.256s", status, rc.buf);
        out = TR_HTTP_FAIL;
    } else {
        char *text = parse_text(rc.buf);
        if (text) {
            *out_text = text;
        } else {
            out = TR_PARSE_FAIL;
        }
    }
    heap_caps_free(rc.buf);
    return out;
}
