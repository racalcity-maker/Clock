#include "alarm_timer.h"

#include "clock_time.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "alarm_timer";

static alarm_event_cb_t s_cb = NULL;
static TaskHandle_t s_alarm_task_handle = NULL;
static const TickType_t ALARM_TICK_DELAY = pdMS_TO_TICKS(1000);

static struct {
    uint8_t hour;
    uint8_t min;
    bool enabled;
    alarm_mode_t mode;
    int last_hour;
    int last_min;
} s_alarm = {0};

static volatile bool s_alarm_suppressed = false;

static struct {
    bool running;
    uint32_t remaining;
} s_countdown = {0};

static void alarm_timer_tick(void)
{
    struct tm now;
    clock_time_get(&now);

    if (s_alarm_suppressed) {
        return;
    }

    if (s_alarm.enabled) {
        bool day_match = true;
        if (s_alarm.mode == ALARM_MODE_WEEKDAYS) {
            day_match = (now.tm_wday >= 1 && now.tm_wday <= 5);
        }
        if (now.tm_hour == s_alarm.hour && now.tm_min == s_alarm.min) {
            if (day_match && (s_alarm.last_hour != now.tm_hour || s_alarm.last_min != now.tm_min)) {
                s_alarm.last_hour = now.tm_hour;
                s_alarm.last_min = now.tm_min;
                if (s_cb) {
                    s_cb(ALARM_EVENT_TRIGGER);
                }
                if (s_alarm.mode == ALARM_MODE_ONCE) {
                    s_alarm.enabled = false;
                }
            }
        }
    }

    if (s_countdown.running) {
        if (s_countdown.remaining > 0) {
            s_countdown.remaining--;
        }
        if (s_countdown.remaining == 0) {
            s_countdown.running = false;
            if (s_cb) {
                s_cb(TIMER_EVENT_DONE);
            }
        }
    }
}

static void alarm_timer_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(ALARM_TICK_DELAY);
        alarm_timer_tick();
    }
}

void alarm_timer_init(alarm_event_cb_t cb)
{
    s_cb = cb;
    if (s_alarm_task_handle) {
        return;
    }
    if (xTaskCreate(alarm_timer_task, "alarm_timer", 4096, NULL, 5, &s_alarm_task_handle) != pdPASS) {
        s_alarm_task_handle = NULL;
        ESP_LOGE(TAG, "alarm timer task create failed");
    }
}

void alarm_set(uint8_t hour, uint8_t min, bool enabled, alarm_mode_t mode)
{
    s_alarm.hour = hour;
    s_alarm.min = min;
    s_alarm.enabled = enabled;
    s_alarm.mode = mode;
    s_alarm.last_hour = -1;
    s_alarm.last_min = -1;
    if (enabled) {
        struct tm now;
        clock_time_get(&now);
        if (now.tm_hour == hour && now.tm_min == min) {
            s_alarm.last_hour = now.tm_hour;
            s_alarm.last_min = now.tm_min;
        }
    }
}

void timer_start(uint32_t seconds)
{
    s_countdown.remaining = seconds;
    s_countdown.running = (seconds > 0);
}

void timer_stop(void)
{
    s_countdown.running = false;
    s_countdown.remaining = 0;
}

void alarm_timer_set_suppressed(bool suppressed)
{
    s_alarm_suppressed = suppressed;
}

