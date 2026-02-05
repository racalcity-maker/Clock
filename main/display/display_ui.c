#include "display_ui.h"

#include "display_74hc595.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static struct {
    uint8_t hours;
    uint8_t minutes;
    bool colon;
} s_time = {0};

typedef enum {
    OVERLAY_NONE,
    OVERLAY_TEXT,
    OVERLAY_DIGITS,
    OVERLAY_SEGMENTS
} overlay_type_t;

static char s_overlay[4] = {' ', ' ', ' ', ' '};
static uint8_t s_overlay_digits[4] = {0};
static uint8_t s_overlay_segs[4] = {0};
static bool s_overlay_colon = false;
static overlay_type_t s_overlay_type = OVERLAY_NONE;
static bool s_overlay_active = false;
static int64_t s_overlay_until_us = 0;
static SemaphoreHandle_t s_overlay_mutex = NULL;

static void overlay_lock(void)
{
    if (s_overlay_mutex) {
        xSemaphoreTake(s_overlay_mutex, portMAX_DELAY);
    }
}

static void overlay_unlock(void)
{
    if (s_overlay_mutex) {
        xSemaphoreGive(s_overlay_mutex);
    }
}

void display_ui_init(void)
{
    if (!s_overlay_mutex) {
        s_overlay_mutex = xSemaphoreCreateMutex();
    }
    s_time.hours = 0;
    s_time.minutes = 0;
    s_time.colon = false;
    s_overlay_active = false;
    s_overlay_type = OVERLAY_NONE;
    s_overlay_until_us = 0;
}

void display_ui_set_time(uint8_t hours, uint8_t minutes, bool colon)
{
    s_time.hours = hours;
    s_time.minutes = minutes;
    s_time.colon = colon;
}

void display_ui_show_text(const char text[4], uint32_t duration_ms)
{
    overlay_lock();
    for (int i = 0; i < 4; ++i) {
        s_overlay[i] = ' ';
    }
    if (text) {
        for (int i = 0; i < 4 && text[i] != '\0'; ++i) {
            s_overlay[i] = text[i];
        }
    }

    if (duration_ms == 0) {
        s_overlay_active = false;
        s_overlay_type = OVERLAY_NONE;
        s_overlay_until_us = 0;
        overlay_unlock();
        return;
    }

    s_overlay_type = OVERLAY_TEXT;
    s_overlay_active = true;
    s_overlay_until_us = esp_timer_get_time() + (int64_t)duration_ms * 1000;
    overlay_unlock();
}

void display_ui_show_digits(const uint8_t digits[4], bool colon, uint32_t duration_ms)
{
    overlay_lock();
    if (digits) {
        for (int i = 0; i < 4; ++i) {
            s_overlay_digits[i] = digits[i];
        }
    } else {
        for (int i = 0; i < 4; ++i) {
            s_overlay_digits[i] = 0;
        }
    }
    s_overlay_colon = colon;

    if (duration_ms == 0) {
        s_overlay_active = false;
        s_overlay_type = OVERLAY_NONE;
        s_overlay_until_us = 0;
        overlay_unlock();
        return;
    }

    s_overlay_type = OVERLAY_DIGITS;
    s_overlay_active = true;
    s_overlay_until_us = esp_timer_get_time() + (int64_t)duration_ms * 1000;
    overlay_unlock();
}

void display_ui_show_segments(const uint8_t segs[4], bool colon, uint32_t duration_ms)
{
    overlay_lock();
    if (segs) {
        for (int i = 0; i < 4; ++i) {
            s_overlay_segs[i] = segs[i];
        }
    } else {
        for (int i = 0; i < 4; ++i) {
            s_overlay_segs[i] = 0;
        }
    }
    s_overlay_colon = colon;

    if (duration_ms == 0) {
        s_overlay_active = false;
        s_overlay_type = OVERLAY_NONE;
        s_overlay_until_us = 0;
        overlay_unlock();
        return;
    }

    s_overlay_type = OVERLAY_SEGMENTS;
    s_overlay_active = true;
    s_overlay_until_us = esp_timer_get_time() + (int64_t)duration_ms * 1000;
    overlay_unlock();
}

void display_ui_render(void)
{
    bool active = false;
    overlay_type_t type = OVERLAY_NONE;
    int64_t until_us = 0;
    bool colon = false;
    char text[4];
    uint8_t digits[4];
    uint8_t segs[4];

    overlay_lock();
    active = s_overlay_active;
    type = s_overlay_type;
    until_us = s_overlay_until_us;
    colon = s_overlay_colon;
    memcpy(text, s_overlay, sizeof(text));
    memcpy(digits, s_overlay_digits, sizeof(digits));
    memcpy(segs, s_overlay_segs, sizeof(segs));
    overlay_unlock();

    if (active) {
        int64_t now = esp_timer_get_time();
        if (now < until_us) {
            if (type == OVERLAY_DIGITS) {
                display_set_digits(digits, colon);
            } else if (type == OVERLAY_SEGMENTS) {
                display_set_segments(segs, colon);
            } else {
                display_set_text(text, false);
            }
            return;
        }
        overlay_lock();
        s_overlay_active = false;
        s_overlay_type = OVERLAY_NONE;
        overlay_unlock();
    }

    if (s_time.hours <= 23 && s_time.minutes <= 59) {
        display_set_time(s_time.hours, s_time.minutes, s_time.colon);
    } else {
        display_set_text("----", true);
    }
}

bool display_ui_overlay_active(void)
{
    bool active = false;
    overlay_lock();
    active = s_overlay_active;
    overlay_unlock();
    return active;
}

bool display_ui_overlay_is_segments(void)
{
    bool is_segments = false;
    overlay_lock();
    is_segments = s_overlay_active && s_overlay_type == OVERLAY_SEGMENTS;
    overlay_unlock();
    return is_segments;
}

