// settings.h — persistent config (Wi-Fi creds + DashScope API key) in NVS
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SETTINGS_STR_MAX 96

#define SETTINGS_URL_MAX 160

typedef struct {
    char wifi_ssid[SETTINGS_STR_MAX];
    char wifi_pass[SETTINGS_STR_MAX];
    char ds_api_key[SETTINGS_STR_MAX];  // DashScope (Aliyun) API key
    char inbox_url[SETTINGS_URL_MAX];   // Notion-relay webhook (Cloudflare Worker) URL
    char inbox_token[SETTINGS_STR_MAX]; // bearer token for the inbox webhook
} glane_settings_t;
void settings_init(void);
const glane_settings_t *settings_get(void);

// Update + persist individual fields (empty string leaves value unchanged
// only for the api key path is handled by caller; here a write always stores).
void settings_set_wifi(const char *ssid, const char *pass);
void settings_set_api_key(const char *key);
void settings_set_inbox(const char *url, const char *token);

bool settings_has_wifi(void);
bool settings_has_api_key(void);
bool settings_has_inbox(void);

// Returns true (once) after wifi/api-key were written, then clears the flag.
bool settings_take_dirty(void);

// Monotonic note sequence counter, persisted in NVS. Returns the next id and
// advances+stores it. Survives deep-sleep and power loss, so note filenames
// never collide (unlike a boot-relative uptime counter).
uint32_t settings_next_note_seq(void);

#ifdef __cplusplus
}
#endif
