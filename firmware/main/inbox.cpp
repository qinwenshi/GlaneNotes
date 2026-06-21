// inbox.cpp — POST a transcribed note to the inbox webhook (Cloudflare Worker
// → Notion). Mirrors the HTTPS client pattern used by transcribe.cpp.
#include "inbox.h"
#include "settings.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "inbox";

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
        if (!r || !r->buf) return ESP_OK;
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

// Format a unix time as ISO-8601 UTC, e.g. "2026-06-21T13:00:00Z".
static void iso_utc(time_t t, char *out, size_t n)
{
    if (t <= 0) { out[0] = '\0'; return; }
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(out, n, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

bool inbox_enabled(void)
{
    return settings_has_inbox();
}

ib_result_t inbox_push(const char *id, time_t created, int duration_s, const char *text)
{
    if (!settings_has_inbox()) return IB_DISABLED;
    const glane_settings_t *cfg = settings_get();

    // Build the JSON payload with cJSON so the transcript is correctly escaped.
    cJSON *root = cJSON_CreateObject();
    if (!root) return IB_OOM;
    cJSON_AddStringToObject(root, "id", id);
    cJSON_AddStringToObject(root, "source", "glane-notes");
    if (duration_s > 0) cJSON_AddNumberToObject(root, "duration_s", duration_s);
    char created_iso[32];
    iso_utc(created, created_iso, sizeof(created_iso));
    if (created_iso[0]) cJSON_AddStringToObject(root, "created", created_iso);
    cJSON_AddStringToObject(root, "text", text ? text : "");

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return IB_OOM;

    struct resp_ctx rc = {};
    rc.cap = 1024;
    rc.buf = (char *)heap_caps_malloc(rc.cap, MALLOC_CAP_SPIRAM);
    if (rc.buf) rc.buf[0] = '\0';

    esp_http_client_config_t hc = {};
    hc.url               = cfg->inbox_url;
    hc.method            = HTTP_METHOD_POST;
    hc.event_handler     = http_evt;
    hc.user_data         = &rc;
    hc.crt_bundle_attach = esp_crt_bundle_attach;
    hc.timeout_ms        = 30000;
    hc.buffer_size       = 2048;
    hc.buffer_size_tx    = 2048;

    esp_http_client_handle_t cli = esp_http_client_init(&hc);
    if (!cli) { free(body); if (rc.buf) heap_caps_free(rc.buf); return IB_HTTP_FAIL; }

    if (cfg->inbox_token[0]) {
        char auth[SETTINGS_STR_MAX + 8];
        snprintf(auth, sizeof(auth), "Bearer %s", cfg->inbox_token);
        esp_http_client_set_header(cli, "Authorization", auth);
    }
    esp_http_client_set_header(cli, "Content-Type", "application/json");
    esp_http_client_set_post_field(cli, body, strlen(body));

    ESP_LOGI(TAG, "POST %s (%s, %u byte body)", cfg->inbox_url, id, (unsigned)strlen(body));
    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);
    free(body);

    ib_result_t out = IB_OK;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "perform failed: %s", esp_err_to_name(err));
        out = IB_HTTP_FAIL;
    } else if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "HTTP %d: %.256s", status, rc.buf ? rc.buf : "");
        out = IB_HTTP_FAIL;
    } else {
        ESP_LOGI(TAG, "%s pushed (HTTP %d)", id, status);
    }
    if (rc.buf) heap_caps_free(rc.buf);
    return out;
}
