#include "alarm_sound.h"

#include "app_control.h"
#include "audio_owner.h"
#include "audio_pcm5102.h"
#include "helix_mp3_wrapper.h"
#include "storage_sd_spi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define ALARM_DIR "/sdcard/alarm"
#define ALARM_MAX_FILES 99
#define ALARM_TASK_STACK 8192
#define ALARM_TASK_PRIORITY 8
#define ALARM_I2S_TIMEOUT_MS 5000

typedef enum {
    ALARM_CMD_PLAY_INDEX,
    ALARM_CMD_PLAY_BUILTIN,
    ALARM_CMD_STOP
} alarm_cmd_type_t;

typedef struct {
    alarm_cmd_type_t type;
    uint8_t index;
    uint8_t tone;
    uint8_t volume_steps;
    uint32_t preview_ms;
} alarm_cmd_t;

typedef struct {
    uint32_t preview_end_ms;
    uint32_t last_elapsed_ms;
    uint32_t last_est_total_ms;
} alarm_progress_ctx_t;

static const char *TAG = "alarm_sound";
static QueueHandle_t s_cmd_queue = NULL;
static TaskHandle_t s_task = NULL;
static volatile bool s_stop_requested = false;
static volatile bool s_playing = false;
static uint8_t s_alarm_vol_percent = 100;

static bool alarm_is_mp3_file(const char *name)
{
    if (!name || name[0] == '\0' || name[0] == '.') {
        return false;
    }
    size_t len = strlen(name);
    if (len < 4) {
        return false;
    }
    const char *ext = name + len - 4;
    return (tolower((unsigned char)ext[0]) == '.' &&
            tolower((unsigned char)ext[1]) == 'm' &&
            tolower((unsigned char)ext[2]) == 'p' &&
            tolower((unsigned char)ext[3]) == '3');
}

static bool alarm_is_regular_file(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    return S_ISREG(st.st_mode);
}

uint8_t alarm_sound_get_file_count(void)
{
    if (!storage_sd_is_mounted()) {
        return 0;
    }
    DIR *dir = opendir(ALARM_DIR);
    if (!dir) {
        return 0;
    }
    uint8_t count = 0;
    struct dirent *ent;
    char path[128];
    while ((ent = readdir(dir)) != NULL) {
        if (!alarm_is_mp3_file(ent->d_name)) {
            continue;
        }
        int len = snprintf(path, sizeof(path), "%s/%s", ALARM_DIR, ent->d_name);
        if (len <= 0 || (size_t)len >= sizeof(path)) {
            continue;
        }
        if (!alarm_is_regular_file(path)) {
            continue;
        }
        count++;
        if (count >= ALARM_MAX_FILES) {
            break;
        }
    }
    closedir(dir);
    return count;
}

static bool alarm_sound_get_path_by_index(uint8_t index, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return false;
    }
    if (!storage_sd_is_mounted()) {
        return false;
    }
    DIR *dir = opendir(ALARM_DIR);
    if (!dir) {
        return false;
    }
    uint8_t count = 0;
    struct dirent *ent;
    char path[128];
    bool found = false;
    while ((ent = readdir(dir)) != NULL) {
        if (!alarm_is_mp3_file(ent->d_name)) {
            continue;
        }
        int len = snprintf(path, sizeof(path), "%s/%s", ALARM_DIR, ent->d_name);
        if (len <= 0 || (size_t)len >= sizeof(path)) {
            continue;
        }
        if (!alarm_is_regular_file(path)) {
            continue;
        }
        count++;
        if (count == index) {
            if ((size_t)len < out_len) {
                memcpy(out, path, (size_t)len + 1);
                found = true;
            }
            break;
        }
        if (count >= ALARM_MAX_FILES) {
            break;
        }
    }
    closedir(dir);
    return found;
}

static void alarm_flush_silence(void)
{
    static const uint8_t zeros[1024] = {0};
    size_t written = 0;
    for (int i = 0; i < 12; ++i) {
        if (s_stop_requested) {
            break;
        }
        audio_i2s_write(zeros, sizeof(zeros), &written, 20);
    }
}

static size_t alarm_mp3_write_cb(const uint8_t *data, size_t len, void *user)
{
    (void)user;
    if (s_stop_requested) {
        return 0;
    }
    size_t written = 0;
    const uint8_t *out = data;
    size_t out_len = len;
    if (s_alarm_vol_percent < 100 && len >= sizeof(int16_t)) {
        static int16_t s_mp3_buf[1152 * 2];
        size_t samples = len / sizeof(int16_t);
        if (samples > (sizeof(s_mp3_buf) / sizeof(s_mp3_buf[0]))) {
            samples = sizeof(s_mp3_buf) / sizeof(s_mp3_buf[0]);
        }
        const int16_t *in = (const int16_t *)data;
        for (size_t i = 0; i < samples; ++i) {
            s_mp3_buf[i] = (int16_t)(((int32_t)in[i] * s_alarm_vol_percent) / 100);
        }
        out = (const uint8_t *)s_mp3_buf;
        out_len = samples * sizeof(int16_t);
    }
    int64_t start_us = esp_timer_get_time();
    if (audio_i2s_write(out, out_len, &written, ALARM_I2S_TIMEOUT_MS) != ESP_OK) {
        int64_t dur_us = esp_timer_get_time() - start_us;
        ESP_LOGW(TAG, "i2s write failed (owner=%s, len=%u, wrote=%u, dt=%lldus)",
                 audio_owner_name(audio_owner_get()),
                 (unsigned)out_len, (unsigned)written, (long long)dur_us);
        return 0;
    }
    if (s_stop_requested) {
        return 0;
    }
    return written;
}

static void alarm_mp3_progress_cb(size_t bytes_read,
                                  size_t total_bytes,
                                  uint32_t elapsed_ms,
                                  uint32_t est_total_ms,
                                  void *user)
{
    (void)bytes_read;
    (void)total_bytes;
    alarm_progress_ctx_t *ctx = (alarm_progress_ctx_t *)user;
    if (!ctx) {
        return;
    }
    if (elapsed_ms > 0) {
        ctx->last_elapsed_ms = elapsed_ms;
    }
    if (est_total_ms > 0) {
        ctx->last_est_total_ms = est_total_ms;
    }
    if (ctx->preview_end_ms > 0 && elapsed_ms >= ctx->preview_end_ms) {
        s_stop_requested = true;
    }
}

static int alarm_volume_percent(uint8_t steps)
{
    if (steps > APP_VOLUME_MAX) {
        steps = APP_VOLUME_MAX;
    }
    return (int)((steps * 100U + (APP_VOLUME_MAX / 2U)) / APP_VOLUME_MAX);
}

static void alarm_play_index(uint8_t index, uint8_t volume_steps, uint32_t preview_ms)
{
    if (index == 0) {
        return;
    }
    char path[128];
    if (!alarm_sound_get_path_by_index(index, path, sizeof(path))) {
        ESP_LOGW(TAG, "alarm file %u not found", (unsigned)index);
        return;
    }

    uint32_t played_ms = 0;
    int vol_percent = alarm_volume_percent(volume_steps);
    s_alarm_vol_percent = (uint8_t)vol_percent;
    audio_i2s_set_sample_rate(44100);
    s_stop_requested = false;

    while (!s_stop_requested && (preview_ms == 0 || played_ms < preview_ms)) {
        uint32_t remaining = (preview_ms > 0) ? (preview_ms - played_ms) : 0;
        alarm_progress_ctx_t ctx = {
            .preview_end_ms = remaining,
            .last_elapsed_ms = 0,
            .last_est_total_ms = 0
        };
        bool ok = helix_mp3_decode_file(path,
                                        vol_percent,
                                        alarm_mp3_write_cb,
                                        NULL,
                                        alarm_mp3_progress_cb,
                                        &ctx,
                                        0.0f);
        if (s_stop_requested) {
            break;
        }
        if (!ok) {
            break;
        }
        if (preview_ms == 0) {
            continue;
        }
        uint32_t span_ms = ctx.last_elapsed_ms;
        if (span_ms == 0 && ctx.last_est_total_ms > 0) {
            span_ms = ctx.last_est_total_ms;
        }
        if (span_ms == 0) {
            span_ms = remaining;
        }
        if (span_ms >= remaining) {
            break;
        }
        played_ms += span_ms;
    }

    if (!s_stop_requested) {
        alarm_flush_silence();
    }
}

static uint32_t alarm_builtin_duration_ms(uint8_t tone)
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

static void alarm_play_builtin(uint8_t tone, uint8_t volume_steps, uint32_t duration_ms)
{
    uint32_t step_ms = alarm_builtin_duration_ms(tone);
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

static void alarm_task(void *arg)
{
    (void)arg;
    alarm_cmd_t cmd;
    while (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) == pdTRUE) {
        if (cmd.type == ALARM_CMD_STOP) {
            s_stop_requested = true;
            continue;
        }
        if (cmd.type == ALARM_CMD_PLAY_INDEX) {
            s_stop_requested = false;
            s_playing = true;
            alarm_play_index(cmd.index, cmd.volume_steps, cmd.preview_ms);
            s_playing = false;
        } else if (cmd.type == ALARM_CMD_PLAY_BUILTIN) {
            s_stop_requested = false;
            s_playing = true;
            alarm_play_builtin(cmd.tone, cmd.volume_steps, cmd.preview_ms);
            s_playing = false;
        }
        s_stop_requested = false;
    }
}

void alarm_sound_init(void)
{
    if (s_cmd_queue) {
        return;
    }
    s_cmd_queue = xQueueCreate(2, sizeof(alarm_cmd_t));
    if (!s_cmd_queue) {
        ESP_LOGW(TAG, "alarm cmd queue create failed");
        return;
    }
    if (xTaskCreate(alarm_task, "alarm_sound", ALARM_TASK_STACK, NULL, ALARM_TASK_PRIORITY, &s_task) != pdPASS) {
        ESP_LOGW(TAG, "alarm task create failed");
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
    }
}

void alarm_sound_stop(void)
{
    s_stop_requested = true;
    audio_stop();
    alarm_flush_silence();
    audio_i2s_write_silence(50);
    audio_i2s_reset();
    audio_owner_release(AUDIO_OWNER_ALARM);
    if (!s_cmd_queue) {
        return;
    }
    alarm_cmd_t cmd = {
        .type = ALARM_CMD_STOP,
        .index = 0,
        .tone = 0,
        .volume_steps = 0,
        .preview_ms = 0
    };
    xQueueReset(s_cmd_queue);
    xQueueSend(s_cmd_queue, &cmd, 0);
}

bool alarm_sound_is_playing(void)
{
    return s_playing;
}

bool alarm_sound_play_index(uint8_t index, uint8_t volume_steps, uint32_t preview_ms)
{
    if (!s_cmd_queue) {
        return false;
    }
    if (!audio_owner_acquire(AUDIO_OWNER_ALARM, true)) {
        return false;
    }
    alarm_cmd_t cmd = {
        .type = ALARM_CMD_PLAY_INDEX,
        .index = index,
        .tone = 0,
        .volume_steps = volume_steps,
        .preview_ms = preview_ms
    };
    s_stop_requested = true;
    xQueueReset(s_cmd_queue);
    return xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE;
}

bool alarm_sound_play_builtin(uint8_t tone, uint8_t volume_steps, uint32_t duration_ms)
{
    if (!s_cmd_queue) {
        return false;
    }
    if (!audio_owner_acquire(AUDIO_OWNER_ALARM, true)) {
        return false;
    }
    alarm_cmd_t cmd = {
        .type = ALARM_CMD_PLAY_BUILTIN,
        .index = 0,
        .tone = tone,
        .volume_steps = volume_steps,
        .preview_ms = duration_ms
    };
    s_stop_requested = true;
    xQueueReset(s_cmd_queue);
    return xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE;
}
