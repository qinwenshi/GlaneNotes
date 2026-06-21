// settings.cpp — NVS-backed configuration store
#include "settings.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>
#include <stdint.h>

static const char *TAG = "settings";
static const char *NS  = "glane";

static glane_settings_t s_cfg;
static volatile bool    s_dirty = false;   // set when wifi/api key is written

// Optional compile-time defaults (set via menuconfig / build flags if desired)
#ifndef GLANE_DEFAULT_WIFI_SSID
#define GLANE_DEFAULT_WIFI_SSID ""
#endif
#ifndef GLANE_DEFAULT_WIFI_PASS
#define GLANE_DEFAULT_WIFI_PASS ""
#endif
#ifndef GLANE_DEFAULT_DS_KEY
#define GLANE_DEFAULT_DS_KEY ""
#endif

static void load_str(nvs_handle_t h, const char *key, char *out, size_t cap, const char *def)
{
    size_t len = cap;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) {
        strncpy(out, def, cap - 1);
        out[cap - 1] = '\0';
    }
}

void settings_init(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        load_str(h, "ssid", s_cfg.wifi_ssid, SETTINGS_STR_MAX, GLANE_DEFAULT_WIFI_SSID);
        load_str(h, "pass", s_cfg.wifi_pass, SETTINGS_STR_MAX, GLANE_DEFAULT_WIFI_PASS);
        load_str(h, "apikey", s_cfg.ds_api_key, SETTINGS_STR_MAX, GLANE_DEFAULT_DS_KEY);
        load_str(h, "inboxurl", s_cfg.inbox_url, SETTINGS_URL_MAX, "");
        load_str(h, "inboxtok", s_cfg.inbox_token, SETTINGS_STR_MAX, "");
        nvs_close(h);
    } else {
        strncpy(s_cfg.wifi_ssid, GLANE_DEFAULT_WIFI_SSID, SETTINGS_STR_MAX - 1);
        strncpy(s_cfg.wifi_pass, GLANE_DEFAULT_WIFI_PASS, SETTINGS_STR_MAX - 1);
        strncpy(s_cfg.ds_api_key, GLANE_DEFAULT_DS_KEY, SETTINGS_STR_MAX - 1);
    }
    ESP_LOGI(TAG, "loaded: ssid='%s' apikey=%s",
             s_cfg.wifi_ssid, s_cfg.ds_api_key[0] ? "set" : "(empty)");
}

const glane_settings_t *settings_get(void)
{
    return &s_cfg;
}

static void store_str(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, key, val);
    nvs_commit(h);
    nvs_close(h);
}

void settings_set_wifi(const char *ssid, const char *pass)
{
    strncpy(s_cfg.wifi_ssid, ssid, SETTINGS_STR_MAX - 1);
    s_cfg.wifi_ssid[SETTINGS_STR_MAX - 1] = '\0';
    strncpy(s_cfg.wifi_pass, pass, SETTINGS_STR_MAX - 1);
    s_cfg.wifi_pass[SETTINGS_STR_MAX - 1] = '\0';
    store_str("ssid", s_cfg.wifi_ssid);
    store_str("pass", s_cfg.wifi_pass);
    s_dirty = true;
}

void settings_set_api_key(const char *key)
{
    strncpy(s_cfg.ds_api_key, key, SETTINGS_STR_MAX - 1);
    s_cfg.ds_api_key[SETTINGS_STR_MAX - 1] = '\0';
    store_str("apikey", s_cfg.ds_api_key);
    s_dirty = true;
}

void settings_set_inbox(const char *url, const char *token)
{
    strncpy(s_cfg.inbox_url, url, SETTINGS_URL_MAX - 1);
    s_cfg.inbox_url[SETTINGS_URL_MAX - 1] = '\0';
    strncpy(s_cfg.inbox_token, token, SETTINGS_STR_MAX - 1);
    s_cfg.inbox_token[SETTINGS_STR_MAX - 1] = '\0';
    store_str("inboxurl", s_cfg.inbox_url);
    store_str("inboxtok", s_cfg.inbox_token);
    s_dirty = true;
}

bool settings_take_dirty(void)
{
    bool d = s_dirty;
    s_dirty = false;
    return d;
}

bool settings_has_wifi(void)    { return s_cfg.wifi_ssid[0] != '\0'; }
bool settings_has_api_key(void) { return s_cfg.ds_api_key[0] != '\0'; }
bool settings_has_inbox(void)   { return s_cfg.inbox_url[0] != '\0'; }

uint32_t settings_next_note_seq(void)
{
    nvs_handle_t h;
    uint32_t seq = 1;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, "noteseq", &seq);   // leaves seq=1 if absent
        uint32_t next = seq + 1;
        nvs_set_u32(h, "noteseq", next);
        nvs_commit(h);
        nvs_close(h);
    }
    return seq;
}
