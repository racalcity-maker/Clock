#pragma once

#include "config_store.h"
#include "esp_err.h"
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_init(const app_config_t *cfg);
bool wifi_is_connected(void);
void wifi_request_time_sync(void);
void wifi_set_enabled(bool enabled);
bool wifi_is_enabled(void);
void wifi_set_web_enabled(bool enabled);
bool wifi_wait_for_shutdown(uint32_t timeout_ms);
bool wifi_is_ap_mode(void);
bool wifi_get_last_sync_time(time_t *out);
esp_err_t wifi_update_credentials(const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif

