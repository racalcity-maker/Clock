#pragma once

#include "config_store.h"
#include "app_control.h"
#include "adc_keys.h"
#include "encoder.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ui_mode_manager_init(app_config_t *cfg, uint8_t *display_brightness, bool *soft_off);
void ui_mode_manager_start(void);
void ui_mode_manager_heap_snapshot(const char *tag);
void ui_mode_manager_set_input_handlers(encoder_event_cb_t encoder_cb, adc_key_event_cb_t adc_cb);
void app_request_input_encoder(encoder_event_t event);
void app_request_input_adc(adc_key_id_t key, adc_key_event_t event);

#ifdef __cplusplus
}
#endif
