#include "ui_time_setting.h"

#include "clock_time.h"
#include "display_74hc595.h"
#include "esp_timer.h"
#include <stdio.h>
#include <sys/time.h>
#include <time.h>

typedef enum {
    TIME_MODE_NORMAL,
    TIME_MODE_SET_HOUR,
    TIME_MODE_SET_MIN
} time_mode_t;

static time_mode_t s_time_mode = TIME_MODE_NORMAL;
static struct tm s_edit_time;
static int64_t s_last_activity_us = 0;
static const int64_t TIME_SETTING_TIMEOUT_US = 10000000;

static int64_t time_now_us(void)
{
    return esp_timer_get_time();
}

static void time_touch(void)
{
    s_last_activity_us = time_now_us();
}

static void time_setting_apply(void)
{
    struct tm tmp = s_edit_time;
    tmp.tm_sec = 0;
    time_t epoch = mktime(&tmp);
    if (epoch < 0) {
        return;
    }
    struct timeval tv = {
        .tv_sec = epoch,
        .tv_usec = 0
    };
    settimeofday(&tv, NULL);
}

void ui_time_setting_render(void)
{
    char text[5];
    if (s_time_mode == TIME_MODE_SET_HOUR) {
        snprintf(text, sizeof(text), "Hr%02u", (unsigned)s_edit_time.tm_hour);
    } else {
        snprintf(text, sizeof(text), "In%02u", (unsigned)s_edit_time.tm_min);
    }
    display_set_text(text, false);
}

void ui_time_setting_enter(void)
{
    clock_time_get(&s_edit_time);
    s_time_mode = TIME_MODE_SET_HOUR;
    time_touch();
    ui_time_setting_render();
}

bool ui_time_setting_is_active(void)
{
    return s_time_mode != TIME_MODE_NORMAL;
}

void ui_time_setting_reset(void)
{
    s_time_mode = TIME_MODE_NORMAL;
    s_last_activity_us = 0;
}

bool ui_time_setting_handle_short_press(void)
{
    if (s_time_mode == TIME_MODE_SET_HOUR) {
        s_time_mode = TIME_MODE_SET_MIN;
        time_touch();
        ui_time_setting_render();
        return true;
    }
    if (s_time_mode == TIME_MODE_SET_MIN) {
        time_setting_apply();
        ui_time_setting_reset();
        return true;
    }
    return false;
}

bool ui_time_setting_handle_knob(int delta)
{
    if (s_time_mode == TIME_MODE_SET_HOUR) {
        int hour = s_edit_time.tm_hour + delta;
        if (hour < 0) {
            hour = 23;
        } else if (hour > 23) {
            hour = 0;
        }
        s_edit_time.tm_hour = hour;
        time_touch();
        time_setting_apply();
        ui_time_setting_render();
        return true;
    }

    if (s_time_mode == TIME_MODE_SET_MIN) {
        int min = s_edit_time.tm_min + delta;
        if (min < 0) {
            min = 59;
        } else if (min > 59) {
            min = 0;
        }
        s_edit_time.tm_min = min;
        time_touch();
        time_setting_apply();
        ui_time_setting_render();
        return true;
    }

    return false;
}

bool ui_time_setting_should_exit(void)
{
    if (s_time_mode == TIME_MODE_NORMAL || s_last_activity_us == 0) {
        return false;
    }
    int64_t now = time_now_us();
    return (now - s_last_activity_us) >= TIME_SETTING_TIMEOUT_US;
}
