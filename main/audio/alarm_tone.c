#include "alarm_tone.h"

#include "app_control.h"
#include "audio_owner.h"
#include "audio_pcm5102.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define ALARM_TONE_TASK_STACK 2048
#define ALARM_TONE_TASK_PRIORITY 8

typedef enum {
    ALARM_TONE_CMD_PLAY,
    ALARM_TONE_CMD_STOP
} alarm_tone_cmd_type_t;

typedef struct {
    alarm_tone_cmd_type_t type;
    uint8_t tone;
    uint8_t volume_steps;
    uint32_t duration_ms;
} alarm_tone_cmd_t;

static QueueHandle_t s_cmd_queue = NULL;
static TaskHandle_t s_task = NULL;
static volatile bool s_stop_requested = false;
static volatile bool s_playing = false;

static uint32_t alarm_tone_step_ms(uint8_t tone)
{
    switch (tone) {
        case 1:
            return 950;
        case 2:
            return 880;
        case 3:
            return 950;
        case 4:
            return 840;
        case 5:
            return 840;
        case 6:
            return 550;
        case 7:
            return 840;
        case 8:
            return 900;
        case 9:
            return 560;
        default:
            return 950;
    }
}

static void alarm_tone_play_internal(uint8_t tone, uint8_t volume_steps, uint32_t duration_ms)
{
    uint32_t step_ms = alarm_tone_step_ms(tone);
    if (step_ms == 0) {
        step_ms = 800;
    }
    uint8_t volume = app_volume_steps_to_byte(volume_steps);
    uint64_t start_us = esp_timer_get_time();

    while (!s_stop_requested) {
        uint64_t elapsed_ms = (esp_timer_get_time() - start_us) / 1000ULL;
        if (duration_ms > 0 && elapsed_ms >= duration_ms) {
            break;
        }
        audio_play_alarm_tone_volume(tone, volume);
        uint32_t wait_ms = step_ms;
        if (duration_ms > 0) {
            uint64_t remaining = duration_ms - elapsed_ms;
            if (remaining < wait_ms) {
                wait_ms = (uint32_t)remaining;
            }
        }
        if (wait_ms == 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}

static void alarm_tone_task(void *arg)
{
    (void)arg;
    alarm_tone_cmd_t cmd;
    while (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) == pdTRUE) {
        if (cmd.type == ALARM_TONE_CMD_STOP) {
            s_stop_requested = true;
            continue;
        }
        if (cmd.type == ALARM_TONE_CMD_PLAY) {
            s_stop_requested = false;
            s_playing = true;
            alarm_tone_play_internal(cmd.tone, cmd.volume_steps, cmd.duration_ms);
            s_playing = false;
        }
        s_stop_requested = false;
    }
}

static bool alarm_tone_ensure_init(void)
{
    if (s_cmd_queue) {
        return true;
    }
    s_cmd_queue = xQueueCreate(2, sizeof(alarm_tone_cmd_t));
    if (!s_cmd_queue) {
        return false;
    }
    if (xTaskCreate(alarm_tone_task, "alarm_tone", ALARM_TONE_TASK_STACK, NULL,
                    ALARM_TONE_TASK_PRIORITY, &s_task) != pdPASS) {
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        s_task = NULL;
        return false;
    }
    return true;
}

bool alarm_tone_is_playing(void)
{
    return s_playing;
}

bool alarm_tone_play(uint8_t tone, uint8_t volume_steps, uint32_t duration_ms)
{
    if (!alarm_tone_ensure_init()) {
        return false;
    }
    if (!audio_owner_acquire(AUDIO_OWNER_ALARM, true)) {
        return false;
    }
    alarm_tone_cmd_t cmd = {
        .type = ALARM_TONE_CMD_PLAY,
        .tone = tone,
        .volume_steps = volume_steps,
        .duration_ms = duration_ms
    };
    s_stop_requested = true;
    xQueueReset(s_cmd_queue);
    return xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE;
}

void alarm_tone_stop(void)
{
    s_stop_requested = true;
    audio_stop();
    audio_i2s_write_silence(50);
    audio_i2s_reset();
    audio_owner_release(AUDIO_OWNER_ALARM);
    if (!s_cmd_queue) {
        return;
    }
    alarm_tone_cmd_t cmd = {
        .type = ALARM_TONE_CMD_STOP,
        .tone = 0,
        .volume_steps = 0,
        .duration_ms = 0
    };
    xQueueReset(s_cmd_queue);
    xQueueSend(s_cmd_queue, &cmd, 0);
}
