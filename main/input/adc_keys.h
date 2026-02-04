#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ADC_KEY_NONE = 0,
    ADC_KEY_POWER,
    ADC_KEY_MODE,
    ADC_KEY_NEXT,
    ADC_KEY_PREV,
    ADC_KEY_BT
} adc_key_id_t;

typedef enum {
    ADC_KEY_EVENT_SHORT,
    ADC_KEY_EVENT_LONG
} adc_key_event_t;

typedef void (*adc_key_event_cb_t)(adc_key_id_t key, adc_key_event_t event);

void adc_keys_init(adc_key_event_cb_t cb);
void adc_keys_deinit(void);

#ifdef __cplusplus
}
#endif
