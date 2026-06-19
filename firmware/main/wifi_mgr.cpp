// wifi_mgr.cpp — Wi-Fi station bring-up & connection management
#include "wifi_mgr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "wifi";

static EventGroupHandle_t s_events = nullptr;
#define BIT_CONNECTED  BIT0
#define BIT_FAILED     BIT1

static esp_netif_t *s_netif    = nullptr;
static esp_netif_t *s_ap_netif = nullptr;
static bool         s_inited   = false;
static volatile bool s_connected = false;
static char         s_ip[16]   = {0};
static int          s_retries  = 0;
static const int    MAX_RETRY  = 5;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        s_ip[0] = '\0';
        if (s_retries < MAX_RETRY) {
            s_retries++;
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_events, BIT_FAILED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_retries   = 0;
        s_connected = true;
        xEventGroupSetBits(s_events, BIT_CONNECTED);
        ESP_LOGI(TAG, "got ip %s", s_ip);
    }
}

void wifi_mgr_init(void)
{
    if (s_inited) return;
    s_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, nullptr, nullptr));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    s_inited = true;
}

bool wifi_mgr_connect(const char *ssid, const char *pass, int timeout_ms)
{
    if (!s_inited) wifi_mgr_init();
    if (!ssid || !ssid[0]) return false;

    // (Re)configure and start.
    esp_wifi_stop();
    wifi_config_t wc = {};
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, pass ? pass : "", sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));

    s_retries   = 0;
    s_connected = false;
    xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_FAILED);
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(
        s_events, BIT_CONNECTED | BIT_FAILED, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    return (bits & BIT_CONNECTED) != 0;
}

void wifi_mgr_disconnect(void)
{
    if (!s_inited) return;
    esp_wifi_disconnect();
    esp_wifi_stop();
    s_connected = false;
    s_ip[0] = '\0';
}

bool wifi_mgr_is_connected(void) { return s_connected; }
const char *wifi_mgr_ip(void)    { return s_ip; }

// ── SoftAP provisioning ──────────────────────────────────────────────────────
const char *wifi_mgr_start_ap(const char *ssid, const char *pass)
{
    static char ap_ip[16] = "192.168.4.1";
    if (!s_inited) wifi_mgr_init();

    esp_wifi_stop();
    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();

    bool open = (!pass || strlen(pass) < 8);   // WPA2 requires >= 8 chars
    wifi_config_t ap = {};
    strncpy((char *)ap.ap.ssid, ssid, sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len = strlen((char *)ap.ap.ssid);
    ap.ap.channel  = 1;
    ap.ap.max_connection = 4;
    if (open) {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    } else {
        strncpy((char *)ap.ap.password, pass, sizeof(ap.ap.password) - 1);
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Report the actual gateway IP of the AP netif.
    esp_netif_ip_info_t info;
    if (s_ap_netif && esp_netif_get_ip_info(s_ap_netif, &info) == ESP_OK) {
        snprintf(ap_ip, sizeof(ap_ip), IPSTR, IP2STR(&info.ip));
    }
    ESP_LOGI(TAG, "SoftAP '%s' up, ip %s (%s)", ssid, ap_ip, open ? "open" : "wpa2");
    return ap_ip;
}

void wifi_mgr_stop_ap(void)
{
    if (!s_inited) return;
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
}
