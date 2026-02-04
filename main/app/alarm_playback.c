#include "alarm_playback.h"

#include "alarm_sound.h"
#include "audio_pcm5102.h"
#include "app_control.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define ALARM_PLAY_MS 120000
#define ALARM_REPEAT_INTERVAL_MS 300000
#define ALARM_REPEAT_MAX 5

static const char *TAG = "alarm_playback";
static esp_timer_handle_t s_repeat_timer = NULL;
static esp_timer_handle_t s_stop_timer = NULL;
static QueueHandle_t s_alarm_queue = NULL;
static TaskHandle_t s_alarm_task = NULL;
static volatile bool s_active = false;
static uint8_t s_repeat_total = 1;
static uint8_t s_repeat_done = 0;
static uint8_t s_alarm_tone = 1;
static uint8_t s_alarm_volume = 1;

static void alarm_play_cycle(void);
static void alarm_playback_task(void *arg);

typedef enum {
    ALARM_CMD_PLAY_CYCLE,
    ALARM_CMD_STOP_AUDIO,
    ALARM_CMD_STOP
} alarm_cmd_t;

static void alarm_repeat_cb(void *arg)
{
    (void)arg;
    if (!s_active) {
        return;
    }
    if (s_alarm_queue) {
        alarm_cmd_t cmd = ALARM_CMD_PLAY_CYCLE;
        xQueueSend(s_alarm_queue, &cmd, 0);
    }
}

static void alarm_stop_cb(void *arg)
{
    (void)arg;
    if (s_alarm_queue) {
        alarm_cmd_t cmd = ALARM_CMD_STOP_AUDIO;
        xQueueSend(s_alarm_queue, &cmd, 0);
    }
}

void alarm_playback_init(void)
{
    if (!s_alarm_queue) {
        s_alarm_queue = xQueueCreate(4, sizeof(alarm_cmd_t));
    }
    if (s_alarm_queue && !s_alarm_task) {
        if (xTaskCreate(alarm_playback_task, "alarm_playback", 4096, NULL, 7, &s_alarm_task) != pdPASS) {
            s_alarm_task = NULL;
            vQueueDelete(s_alarm_queue);
            s_alarm_queue = NULL;
            return;
        }
    }

    if (s_repeat_timer) {
        return;
    }
    const esp_timer_create_args_t args = {
        .callback = &alarm_repeat_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "alarm_repeat"
    };
    esp_timer_create(&args, &s_repeat_timer);

    if (!s_stop_timer) {
        const esp_timer_create_args_t stop_args = {
            .callback = &alarm_stop_cb,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "alarm_stop"
        };
        esp_timer_create(&stop_args, &s_stop_timer);
    }
}

static void alarm_stop_repeat_timer(void)
{
    if (!s_repeat_timer) {
        return;
    }
    if (esp_timer_is_active(s_repeat_timer)) {
        esp_timer_stop(s_repeat_timer);
    }
}

static void alarm_start_repeat_timer(void)
{
    if (!s_repeat_timer) {
        return;
    }
    if (s_repeat_done >= s_repeat_total) {
        return;
    }
    esp_timer_start_once(s_repeat_timer, (uint64_t)ALARM_REPEAT_INTERVAL_MS * 1000ULL);
}

static void alarm_start_stop_timer(void)
{
    if (!s_stop_timer) {
        return;
    }
    if (esp_timer_is_active(s_stop_timer)) {
        esp_timer_stop(s_stop_timer);
    }
    esp_timer_start_once(s_stop_timer, (uint64_t)ALARM_PLAY_MS * 1000ULL);
}

static void alarm_stop_stop_timer(void)
{
    if (!s_stop_timer) {
        return;
    }
    if (esp_timer_is_active(s_stop_timer)) {
        esp_timer_stop(s_stop_timer);
    }
}

static void alarm_play_cycle(void)
{
    if (!s_active) {
        return;
    }
    if (s_repeat_done >= s_repeat_total) {
        return;
    }
    s_repeat_done++;

    alarm_sound_stop();

    bool force_tone = (app_get_ui_mode() == APP_UI_MODE_BLUETOOTH);
    uint8_t count = force_tone ? 0 : alarm_sound_get_file_count();
    if (!force_tone && count > 0) {
        uint8_t index = s_alarm_tone;
        if (index < 1) {
            index = 1;
        } else if (index > count) {
            index = count;
        }
        alarm_sound_play_index(index, s_alarm_volume, 0);
    } else {
        uint8_t tone = s_alarm_tone;
        if (tone < 1) {
            tone = 1;
        } else if (tone > 9) {
            tone = 1;
        }
        audio_i2s_write_silence(50);
        audio_i2s_reset();
        alarm_sound_play_builtin(tone, s_alarm_volume, 0);
    }

    alarm_start_stop_timer();
    alarm_start_repeat_timer();
}

static void alarm_playback_task(void *arg)
{
    (void)arg;
    alarm_cmd_t cmd;
    while (xQueueReceive(s_alarm_queue, &cmd, portMAX_DELAY) == pdTRUE) {
        if (cmd == ALARM_CMD_PLAY_CYCLE) {
            alarm_play_cycle();
        } else if (cmd == ALARM_CMD_STOP_AUDIO) {
            alarm_sound_stop();
            audio_stop();
            audio_i2s_write_silence(50);
            audio_i2s_reset();
        } else if (cmd == ALARM_CMD_STOP) {
            alarm_sound_stop();
            audio_stop();
            audio_i2s_write_silence(50);
            audio_i2s_reset();
            s_repeat_done = 0;
        }
    }
}

void alarm_playback_start(const app_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    if (s_active) {
        return;
    }
    if (!s_repeat_timer) {
        alarm_playback_init();
    }
    if (!s_alarm_queue || !s_alarm_task) {
        alarm_playback_init();
    }

    s_alarm_tone = cfg->alarm_tone;
    s_alarm_volume = cfg->alarm_volume;

    uint8_t repeat = cfg->alarm_repeat;
    if (repeat < 1) {
        repeat = 1;
    } else if (repeat > ALARM_REPEAT_MAX) {
        repeat = ALARM_REPEAT_MAX;
    }
    s_repeat_total = repeat;
    s_repeat_done = 0;
    s_active = true;

    alarm_stop_repeat_timer();
    if (s_alarm_queue) {
        alarm_cmd_t cmd = ALARM_CMD_PLAY_CYCLE;
        if (xQueueSend(s_alarm_queue, &cmd, 0) != pdTRUE) {
            ESP_LOGW(TAG, "queue full; play cycle skipped");
        }
    }
}

void alarm_playback_stop(void)
{
    s_active = false;
    alarm_stop_repeat_timer();
    alarm_stop_stop_timer();
    alarm_sound_stop();
    audio_i2s_write_silence(100);
    audio_i2s_reset();
    if (s_alarm_queue) {
        alarm_cmd_t cmd = ALARM_CMD_STOP;
        xQueueSend(s_alarm_queue, &cmd, 0);
    }
}
