#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t bt_sink_init(const char *device_name);
void bt_sink_deinit(void);
bool bt_sink_is_ready(void);
esp_err_t bt_sink_set_name(const char *device_name);
esp_err_t bt_sink_set_discoverable(bool enabled);
bool bt_sink_is_connected(void);
bool bt_sink_is_playing(void);
bool bt_sink_has_saved_device(void);
esp_err_t bt_sink_try_connect_last(void);
esp_err_t bt_sink_schedule_connect_last(uint32_t delay_ms);
esp_err_t bt_sink_clear_bonds(void);
esp_err_t bt_sink_disconnect(void);
bool bt_sink_is_streaming(void);
void bt_sink_note_audio_data(void);

#ifdef __cplusplus
}
#endif

