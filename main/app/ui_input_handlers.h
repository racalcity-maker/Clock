#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "adc_keys.h"
#include "config_store.h"
#include "encoder.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_input_handlers_init(app_config_t *cfg,
                            uint8_t *volume_level,
                            uint8_t *display_brightness,
                            bool *soft_off,
                            bool *alarm_active);

void ui_input_handle_encoder(encoder_event_t event);
void ui_input_handle_adc_key(adc_key_id_t key, adc_key_event_t event);

#ifdef __cplusplus
}
#endif
