// inbox.h — push a transcribed note to a server-side inbox (PARA capture).
//
// The device never talks to Notion directly. It POSTs a small JSON payload to a
// configurable webhook (a thin Cloudflare Worker, see server/inbox-worker/) which
// holds the Notion token, formats the page and dedups by note id. So the firmware
// only ever needs an `inbox_url` + bearer `inbox_token` and stays stable even if
// the backend (Notion DB, page layout, model) changes.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IB_OK = 0,
    IB_DISABLED,    // no inbox_url configured
    IB_OOM,
    IB_HTTP_FAIL,   // network / non-2xx response
} ib_result_t;

// True when an inbox webhook URL is configured.
bool inbox_enabled(void);

// POST one note to the inbox webhook. `text` is the UTF-8 transcript, `created`
// is the note's wall-clock time (0 = unknown), `duration_s` the audio length.
// A single HTTPS POST; returns IB_OK only on a 2xx response.
ib_result_t inbox_push(const char *id, time_t created, int duration_s, const char *text);

#ifdef __cplusplus
}
#endif
