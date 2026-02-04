#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ALARM_EVENT_TRIGGER,
    TIMER_EVENT_DONE
} alarm_event_t;

typedef enum {
    ALARM_MODE_ONCE = 0,
    ALARM_MODE_WEEKDAYS,
    ALARM_MODE_DAILY
} alarm_mode_t;

typedef void (*alarm_event_cb_t)(alarm_event_t event);

void alarm_timer_init(alarm_event_cb_t cb);
void alarm_timer_set_suppressed(bool suppressed);
void alarm_set(uint8_t hour, uint8_t min, bool enabled, alarm_mode_t mode);
void timer_start(uint32_t seconds);
void timer_stop(void);

#ifdef __cplusplus
}
#endif

