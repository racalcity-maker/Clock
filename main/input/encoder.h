#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ENC_EVENT_CW,
    ENC_EVENT_CCW,
    ENC_EVENT_BTN_SHORT,
    ENC_EVENT_BTN_LONG
} encoder_event_t;

typedef void (*encoder_event_cb_t)(encoder_event_t event);

void encoder_init(encoder_event_cb_t cb);
void encoder_deinit(void);

#ifdef __cplusplus
}
#endif
