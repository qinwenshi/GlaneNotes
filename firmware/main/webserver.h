// webserver.h — local dashboard (esp_http_server)
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Request a sync via the web UI. Implemented in app_main (sets a flag the
// main state machine acts on, so HTTP handlers never block for minutes).
typedef void (*web_sync_request_cb_t)(void);

void webserver_start(web_sync_request_cb_t on_sync_request);
void webserver_stop(void);
bool webserver_is_running(void);

#ifdef __cplusplus
}
#endif
