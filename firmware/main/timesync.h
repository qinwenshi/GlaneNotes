// timesync.h — one-shot SNTP time sync so notes get real timestamps.
#pragma once
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start SNTP (idempotent). Call after Wi-Fi is connected. Sets TZ to China time.
void timesync_start(void);

// True once the clock looks real (past 2025-01-01), i.e. SNTP has set it or the
// RTC retained a previously-synced time across deep sleep.
bool timesync_is_valid(void);

// Format a note timestamp from a unix time into "MM-DD HH:MM" (or "--:--" when
// the time is not valid). buf should be >= 12 bytes.
void timesync_format(time_t t, char *buf, int n);

#ifdef __cplusplus
}
#endif
