// ui.h — minimal e-ink status screens for Glane Notes
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "sync.h"   // note_info_t

#ifdef __cplusplus
extern "C" {
#endif

// Initialise the e-ink panel. Call after EPD power rail is on.
void ui_init(void);

// Full status screen variants.
void ui_show_idle(int note_count, int wifi_connected, const char *ip, int battery_pct);
void ui_show_recording(uint32_t elapsed_sec);
void ui_show_message(const char *line1, const char *line2);

// Wi-Fi provisioning screen: shows the SoftAP SSID to join and the URL to open.
void ui_show_provision(const char *ssid, const char *url);
void ui_show_syncing(int done, int total);

// Notes browser. `items` newest-first; `selected` is the absolute index into
// items; `top` is the index of the first visible row.
void ui_show_list(const note_info_t *items, int count, int selected, int top);

// Single-note detail (metadata + actions). `number` is the display number.
void ui_show_detail(const note_info_t *item, int number);

// Playback screen for the given note number.
void ui_show_playing(int number);

#ifdef __cplusplus
}
#endif
