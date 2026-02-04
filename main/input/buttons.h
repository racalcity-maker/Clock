#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BUTTON_1,
    BUTTON_2,
    BUTTON_3,
    BUTTON_4
} button_id_t;

typedef enum {
    BUTTON_EVENT_SHORT,
    BUTTON_EVENT_LONG
} button_event_t;

typedef void (*button_event_cb_t)(button_id_t button, button_event_t event);

void buttons_init(button_event_cb_t cb);

#ifdef __cplusplus
}
#endif

