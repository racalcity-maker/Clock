#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t led_indicator_init(void);
void led_indicator_set_rgb(uint8_t r, uint8_t g, uint8_t b);
void led_indicator_set_seconds_rgb(uint8_t r, uint8_t g, uint8_t b);
void led_indicator_set_seconds_enabled(bool enabled);

#ifdef __cplusplus
}
#endif

