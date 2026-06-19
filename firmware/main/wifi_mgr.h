// wifi_mgr.h — Wi-Fi station manager
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void wifi_mgr_init(void);                 // init netif/event loop/wifi (STA)
bool wifi_mgr_connect(const char *ssid, const char *pass, int timeout_ms);
void wifi_mgr_disconnect(void);
bool wifi_mgr_is_connected(void);
const char *wifi_mgr_ip(void);            // "x.x.x.x" or "" if not connected

// SoftAP provisioning: host an open/WPA2 access point so a phone can reach the
// built-in web dashboard with no prior network. Returns the AP IP (e.g.
// "192.168.4.1"). `pass` may be NULL/"" for an open network (WPA2 needs >=8 ch).
const char *wifi_mgr_start_ap(const char *ssid, const char *pass);
void wifi_mgr_stop_ap(void);

#ifdef __cplusplus
}
#endif
