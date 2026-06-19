// timesync.cpp — SNTP client + timestamp helpers.
//
// The ESP32-S3 keeps its system clock running in the RTC domain across deep
// sleep, so once SNTP has set the time the timestamps survive sleep/wake (until
// a full power loss). We only treat the clock as valid once it is clearly past
// 2025-01-01, otherwise an un-synced 1970/1980 epoch would show as a bogus date.

#include "timesync.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "timesync";
#define VALID_AFTER 1735689600  // 2025-01-01 00:00:00 UTC

void timesync_start(void)
{
    // China standard time (UTC+8), no DST.
    setenv("TZ", "CST-8", 1);
    tzset();

    if (esp_sntp_enabled()) return;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started");
}

bool timesync_is_valid(void)
{
    return time(NULL) > VALID_AFTER;
}

void timesync_format(time_t t, char *buf, int n)
{
    if (t <= VALID_AFTER) {
        snprintf(buf, n, "--:--");
        return;
    }
    struct tm tmv;
    localtime_r(&t, &tmv);
    snprintf(buf, n, "%02d-%02d %02d:%02d",
             tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
}
