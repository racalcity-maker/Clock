#pragma once

#include "adc_keys.h"
#include "encoder.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_input_init(encoder_event_cb_t encoder_cb, adc_key_event_cb_t adc_cb);
void ui_input_deinit(void);

#ifdef __cplusplus
}
#endif
