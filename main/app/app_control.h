#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_UI_MODE_CLOCK = 0,
    APP_UI_MODE_PLAYER = 1,
    APP_UI_MODE_BLUETOOTH = 2,
    APP_UI_MODE_RADIO = 3
} app_ui_mode_t;

#define APP_VOLUME_MAX 30U

static inline uint8_t app_volume_steps_from_byte(uint8_t volume)
{
    uint32_t step = ((uint32_t)volume * APP_VOLUME_MAX + 127U) / 255U;
    if (step > APP_VOLUME_MAX) {
        step = APP_VOLUME_MAX;
    }
    return (uint8_t)step;
}

static inline uint8_t app_volume_steps_to_byte(uint8_t steps)
{
    if (steps > APP_VOLUME_MAX) {
        steps = APP_VOLUME_MAX;
    }
    return (uint8_t)((steps * 255U + (APP_VOLUME_MAX / 2U)) / APP_VOLUME_MAX);
}

app_ui_mode_t app_get_ui_mode(void);
void app_set_ui_mode(app_ui_mode_t mode);
void app_request_ui_mode(app_ui_mode_t mode);
bool app_ui_is_busy(void);
void app_ui_set_busy(bool busy);
void app_ui_busy_for_ms(uint32_t duration_ms);

#ifdef __cplusplus
}
#endif
