#include "audio_player.h"

#include "audio_owner.h"
#include "audio_pcm5102.h"
#include "helix_mp3_wrapper.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PLAYER_MAX_TRACKS 64
#define PLAYER_MAX_PATH 160
#define PLAYER_QUEUE_DEPTH 8
#define PLAYER_READ_BYTES 1024
#define PLAYER_I2S_TIMEOUT_MS 100
#define PLAYER_MP3_I2S_TIMEOUT_MS 5000
#define PLAYER_DECODE_CORE 1

static const char *TAG = "audio_player";

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint32_t data_offset;
    uint32_t data_size;
} wav_info_t;

typedef enum {
    PLAYER_FMT_UNKNOWN = 0,
    PLAYER_FMT_WAV,
    PLAYER_FMT_MP3
} player_format_t;

typedef enum {
    CMD_PLAY,
    CMD_PAUSE,
    CMD_STOP,
    CMD_NEXT,
    CMD_PREV,
    CMD_RESCAN,
    CMD_SET_REPEAT,
    CMD_SHUTDOWN
} audio_player_cmd_type_t;

typedef struct {
    audio_player_cmd_type_t type;
    audio_repeat_mode_t repeat_mode;
} audio_player_cmd_t;

typedef enum {
    REQ_NONE,
    REQ_STOP,
    REQ_NEXT,
    REQ_PREV
} audio_request_t;

static QueueHandle_t s_cmd_queue = NULL;
static TaskHandle_t s_player_task = NULL;
static SemaphoreHandle_t s_api_mutex = NULL;
static char s_folder[PLAYER_MAX_PATH] = "/sdcard/music";
static char s_current_path[PLAYER_MAX_PATH];
static uint16_t s_track_count = 0;
static uint16_t s_order[PLAYER_MAX_TRACKS];
static uint16_t s_order_index = 0;
static audio_repeat_mode_t s_repeat_mode = PLAYER_REPEAT_ALL;
static audio_player_state_t s_state = PLAYER_STATE_STOPPED;
static audio_request_t s_request = REQ_NONE;
static uint8_t s_volume = 200;
static volatile uint32_t s_elapsed_ms = 0;
static volatile uint32_t s_total_ms = 0;
static volatile bool s_shutdown_requested = false;

static void player_lock(void)
{
    if (s_api_mutex) {
        xSemaphoreTake(s_api_mutex, portMAX_DELAY);
    }
}

static void player_unlock(void)
{
    if (s_api_mutex) {
        xSemaphoreGive(s_api_mutex);
    }
}

static bool player_has_tracks(void)
{
    return s_track_count > 0;
}

static player_format_t player_detect_format(const char *name)
{
    size_t len = strlen(name);
    if (len < 4) {
        return PLAYER_FMT_UNKNOWN;
    }
    const char *ext = name + len - 4;
    if ((tolower((unsigned char)ext[0]) == '.') &&
        (tolower((unsigned char)ext[1]) == 'w') &&
        (tolower((unsigned char)ext[2]) == 'a') &&
        (tolower((unsigned char)ext[3]) == 'v')) {
        return PLAYER_FMT_WAV;
    }
    if ((tolower((unsigned char)ext[0]) == '.') &&
        (tolower((unsigned char)ext[1]) == 'm') &&
        (tolower((unsigned char)ext[2]) == 'p') &&
        (tolower((unsigned char)ext[3]) == '3')) {
        return PLAYER_FMT_MP3;
    }
    return PLAYER_FMT_UNKNOWN;
}

static void player_build_order(void)
{
    for (uint16_t i = 0; i < s_track_count; ++i) {
        s_order[i] = i;
    }

    if (s_repeat_mode != PLAYER_REPEAT_SHUFFLE || s_track_count < 2) {
        s_order_index = (s_order_index < s_track_count) ? s_order_index : 0;
        return;
    }

    for (uint16_t i = s_track_count - 1; i > 0; --i) {
        uint32_t j = esp_random() % (i + 1);
        uint16_t tmp = s_order[i];
        s_order[i] = s_order[j];
        s_order[j] = tmp;
    }
    s_order_index = 0;
}

static const char *player_current_path(void)
{
    if (!player_has_tracks()) {
        return NULL;
    }
    uint16_t track_idx = s_order[s_order_index];
    if (track_idx >= s_track_count) {
        return NULL;
    }
    DIR *dir = opendir(s_folder);
    if (!dir) {
        ESP_LOGW(TAG, "folder open failed: %s", s_folder);
        return NULL;
    }
    struct dirent *entry;
    uint16_t idx = 0;
    const char *result = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        if (player_detect_format(entry->d_name) == PLAYER_FMT_UNKNOWN) {
            continue;
        }
        if (idx == track_idx) {
            int written = snprintf(s_current_path, sizeof(s_current_path), "%s/%s", s_folder, entry->d_name);
            if (written > 0 && written < (int)sizeof(s_current_path)) {
                result = s_current_path;
            }
            break;
        }
        idx++;
    }
    closedir(dir);
    return result;
}

static bool player_step_next(bool manual)
{
    if (!player_has_tracks()) {
        return false;
    }

    if (!manual && s_repeat_mode == PLAYER_REPEAT_ONE) {
        return true;
    }

    if (s_order_index + 1 < s_track_count) {
        s_order_index++;
        return true;
    }

    if (manual || s_repeat_mode == PLAYER_REPEAT_ALL || s_repeat_mode == PLAYER_REPEAT_SHUFFLE) {
        player_build_order();
        s_order_index = 0;
        return true;
    }

    return false;
}

static bool player_step_prev(bool manual)
{
    if (!player_has_tracks()) {
        return false;
    }

    if (!manual && s_repeat_mode == PLAYER_REPEAT_ONE) {
        return true;
    }

    if (s_order_index > 0) {
        s_order_index--;
        return true;
    }

    if (manual || s_repeat_mode == PLAYER_REPEAT_ALL || s_repeat_mode == PLAYER_REPEAT_SHUFFLE) {
        player_build_order();
        if (s_track_count > 0) {
            s_order_index = s_track_count - 1;
        }
        return true;
    }

    return false;
}

static esp_err_t player_scan_folder(void)
{
    s_track_count = 0;

    DIR *dir = opendir(s_folder);
    if (!dir) {
        ESP_LOGW(TAG, "folder open failed: %s", s_folder);
        return ESP_FAIL;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && s_track_count < PLAYER_MAX_TRACKS) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        if (player_detect_format(entry->d_name) == PLAYER_FMT_UNKNOWN) {
            continue;
        }
        s_track_count++;
    }
    closedir(dir);

    player_build_order();
    return ESP_OK;
}

static bool wav_read_header(FILE *fp, wav_info_t *out)
{
    uint8_t header[12];
    if (fread(header, 1, sizeof(header), fp) != sizeof(header)) {
        return false;
    }
    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        return false;
    }

    bool got_fmt = false;
    bool got_data = false;
    uint32_t data_offset = 0;
    uint32_t data_size = 0;
    uint32_t sample_rate = 0;
    uint16_t channels = 0;
    uint16_t bits = 0;

    while (!got_fmt || !got_data) {
        uint8_t chunk[8];
        if (fread(chunk, 1, sizeof(chunk), fp) != sizeof(chunk)) {
            break;
        }

        uint32_t chunk_size = (uint32_t)chunk[4] |
                              ((uint32_t)chunk[5] << 8) |
                              ((uint32_t)chunk[6] << 16) |
                              ((uint32_t)chunk[7] << 24);

        if (memcmp(chunk, "fmt ", 4) == 0) {
            uint8_t fmt[16] = {0};
            size_t to_read = chunk_size < sizeof(fmt) ? chunk_size : sizeof(fmt);
            if (fread(fmt, 1, to_read, fp) != to_read) {
                return false;
            }
            if (chunk_size > to_read) {
                fseek(fp, (long)(chunk_size - to_read), SEEK_CUR);
            }

            uint16_t audio_format = (uint16_t)fmt[0] | ((uint16_t)fmt[1] << 8);
            channels = (uint16_t)fmt[2] | ((uint16_t)fmt[3] << 8);
            sample_rate = (uint32_t)fmt[4] |
                          ((uint32_t)fmt[5] << 8) |
                          ((uint32_t)fmt[6] << 16) |
                          ((uint32_t)fmt[7] << 24);
            bits = (uint16_t)fmt[14] | ((uint16_t)fmt[15] << 8);

            if (audio_format != 1) {
                ESP_LOGW(TAG, "unsupported format: %u", (unsigned)audio_format);
                return false;
            }
            got_fmt = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            data_offset = (uint32_t)ftell(fp);
            data_size = chunk_size;
            fseek(fp, (long)chunk_size, SEEK_CUR);
            got_data = true;
        } else {
            fseek(fp, (long)chunk_size, SEEK_CUR);
        }

        if (chunk_size & 1) {
            fseek(fp, 1, SEEK_CUR);
        }
    }

    if (!got_fmt || !got_data) {
        return false;
    }

    if (bits != 16 || (channels != 1 && channels != 2)) {
        ESP_LOGW(TAG, "unsupported wav: %u bit, %u ch", (unsigned)bits, (unsigned)channels);
        return false;
    }

    out->sample_rate = sample_rate;
    out->channels = channels;
    out->bits_per_sample = bits;
    out->data_offset = data_offset;
    out->data_size = data_size;

    fseek(fp, (long)data_offset, SEEK_SET);
    return true;
}

static void player_handle_cmd(const audio_player_cmd_t *cmd)
{
    switch (cmd->type) {
        case CMD_PLAY:
            if (s_state == PLAYER_STATE_PAUSED) {
                s_state = PLAYER_STATE_PLAYING;
            } else if (s_state == PLAYER_STATE_STOPPED) {
                if (!player_has_tracks()) {
                    player_scan_folder();
                }
                if (player_has_tracks()) {
                    s_state = PLAYER_STATE_PLAYING;
                }
            }
            break;
        case CMD_PAUSE:
            if (s_state == PLAYER_STATE_PLAYING) {
                s_state = PLAYER_STATE_PAUSED;
            }
            break;
        case CMD_STOP:
            s_state = PLAYER_STATE_STOPPED;
            s_request = REQ_STOP;
            break;
        case CMD_NEXT:
            s_request = REQ_NEXT;
            s_state = PLAYER_STATE_PLAYING;
            break;
        case CMD_PREV:
            s_request = REQ_PREV;
            s_state = PLAYER_STATE_PLAYING;
            break;
        case CMD_RESCAN:
            player_scan_folder();
            break;
        case CMD_SET_REPEAT:
            s_repeat_mode = cmd->repeat_mode;
            player_build_order();
            break;
        case CMD_SHUTDOWN:
            s_state = PLAYER_STATE_STOPPED;
            s_request = REQ_STOP;
            s_shutdown_requested = true;
            break;
        default:
            break;
    }
}

static void player_drain_cmds(void)
{
    audio_player_cmd_t cmd;
    while (xQueueReceive(s_cmd_queue, &cmd, 0)) {
        player_handle_cmd(&cmd);
    }
}

static void player_stream_file(FILE *fp, const wav_info_t *info)
{
    size_t bytes_written = 0;
    uint32_t remaining = info->data_size;
    uint32_t in_frame_bytes = info->channels * 2;
    uint32_t total_frames = info->data_size / in_frame_bytes;
    uint32_t processed_frames = 0;
    s_total_ms = (uint32_t)(((uint64_t)total_frames * 1000ULL) / info->sample_rate);
    s_elapsed_ms = 0;

    uint8_t raw[PLAYER_READ_BYTES];
    int16_t out[(PLAYER_READ_BYTES / 2) * 2];

    audio_i2s_set_sample_rate(info->sample_rate);

    while (remaining > 0) {
        player_drain_cmds();
        if (s_request != REQ_NONE || s_state == PLAYER_STATE_STOPPED) {
            break;
        }
        if (s_state == PLAYER_STATE_PAUSED) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        size_t to_read = remaining > sizeof(raw) ? sizeof(raw) : remaining;
        size_t read = fread(raw, 1, to_read, fp);
        if (read == 0) {
            break;
        }
        remaining -= (uint32_t)read;
        size_t frames = read / in_frame_bytes;
        processed_frames += (uint32_t)frames;
        s_elapsed_ms = (uint32_t)(((uint64_t)processed_frames * 1000ULL) / info->sample_rate);

        int16_t *in_samples = (int16_t *)raw;
        for (size_t i = 0; i < frames; ++i) {
            int16_t left;
            int16_t right;
            if (info->channels == 2) {
                left = in_samples[i * 2];
                right = in_samples[i * 2 + 1];
            } else {
                left = in_samples[i];
                right = in_samples[i];
            }

            left = (int16_t)(((int32_t)left * s_volume) / 255);
            right = (int16_t)(((int32_t)right * s_volume) / 255);
            out[i * 2] = left;
            out[i * 2 + 1] = right;
        }

        if (s_request != REQ_NONE || s_state == PLAYER_STATE_STOPPED) {
            break;
        }
        audio_i2s_write(out, frames * sizeof(int16_t) * 2, &bytes_written, PLAYER_I2S_TIMEOUT_MS);
    }
}

static size_t mp3_i2s_write_cb(const uint8_t *data, size_t len, void *user)
{
    (void)user;

    player_drain_cmds();
    while (s_state == PLAYER_STATE_PAUSED && s_request == REQ_NONE) {
        player_drain_cmds();
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (s_request != REQ_NONE || s_state == PLAYER_STATE_STOPPED) {
        return 0;
    }

    size_t bytes_written = 0;
    const uint8_t *out = data;
    size_t out_len = len;
    if (s_volume < 255 && len >= sizeof(int16_t)) {
        static int16_t s_mp3_buf[1152 * 2];
        size_t samples = len / sizeof(int16_t);
        if (samples > (sizeof(s_mp3_buf) / sizeof(s_mp3_buf[0]))) {
            samples = sizeof(s_mp3_buf) / sizeof(s_mp3_buf[0]);
        }
        const int16_t *in = (const int16_t *)data;
        for (size_t i = 0; i < samples; ++i) {
            s_mp3_buf[i] = (int16_t)(((int32_t)in[i] * s_volume) / 255);
        }
        out = (const uint8_t *)s_mp3_buf;
        out_len = samples * sizeof(int16_t);
    }
    esp_err_t err = audio_i2s_write(out, out_len, &bytes_written, PLAYER_MP3_I2S_TIMEOUT_MS);
    if (err != ESP_OK || bytes_written == 0) {
        ESP_LOGW(TAG, "i2s write failed err=%s bytes=%u/%u",
                 esp_err_to_name(err),
                 (unsigned)bytes_written,
                 (unsigned)len);
    }
    return bytes_written;
}

static void mp3_progress_cb(size_t bytes_read, size_t total_bytes, uint32_t elapsed_ms, uint32_t est_total_ms, void *user)
{
    (void)bytes_read;
    (void)total_bytes;
    (void)user;
    if (est_total_ms > 0) {
        s_total_ms = est_total_ms;
    }
    if (elapsed_ms > 0) {
        s_elapsed_ms = elapsed_ms;
    }
}

static void player_task(void *arg)
{
    (void)arg;
    audio_player_cmd_t cmd;

    while (1) {
        if (s_shutdown_requested) {
            break;
        }
        if (s_state != PLAYER_STATE_PLAYING) {
            if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY)) {
                player_handle_cmd(&cmd);
                if (s_shutdown_requested) {
                    break;
                }
            }
            continue;
        }

        const char *path = player_current_path();
        if (!path) {
            s_state = PLAYER_STATE_STOPPED;
            continue;
        }

        player_format_t fmt = player_detect_format(path);
        if (!audio_owner_acquire(AUDIO_OWNER_PLAYER, false)) {
            s_state = PLAYER_STATE_STOPPED;
            s_request = REQ_STOP;
            continue;
        }

        if (fmt == PLAYER_FMT_MP3) {
            audio_stop();
            audio_i2s_set_sample_rate(44100);
            int volume_percent = (int)((s_volume * 100U + 127U) / 255U);
            s_elapsed_ms = 0;
            s_total_ms = 0;
            bool ok = helix_mp3_decode_file(path, volume_percent, mp3_i2s_write_cb, NULL, mp3_progress_cb, NULL, 0.0f);
            if (!ok && s_request == REQ_NONE) {
                s_request = REQ_NEXT;
            }
        } else if (fmt == PLAYER_FMT_WAV) {
            FILE *fp = fopen(path, "rb");
            if (!fp) {
                ESP_LOGW(TAG, "file open failed: %s", path);
                s_request = REQ_NEXT;
            } else {
                wav_info_t info = {0};
                if (wav_read_header(fp, &info)) {
                    audio_stop();
                    player_stream_file(fp, &info);
                } else {
                    ESP_LOGW(TAG, "wav parse failed: %s", path);
                    s_request = REQ_NEXT;
                }
                fclose(fp);
            }
        } else {
            ESP_LOGW(TAG, "unsupported format: %s", path);
            s_request = REQ_NEXT;
        }

        audio_request_t req = s_request;
        s_request = REQ_NONE;
        audio_owner_release(AUDIO_OWNER_PLAYER);

        if (req == REQ_STOP) {
            s_state = PLAYER_STATE_STOPPED;
            s_elapsed_ms = 0;
            s_total_ms = 0;
            if (s_shutdown_requested) {
                break;
            }
            continue;
        }

        if (req == REQ_NEXT) {
            if (!player_step_next(true)) {
                s_state = PLAYER_STATE_STOPPED;
                s_elapsed_ms = 0;
                s_total_ms = 0;
            }
            if (s_shutdown_requested) {
                break;
            }
            continue;
        }

        if (req == REQ_PREV) {
            if (!player_step_prev(true)) {
                s_state = PLAYER_STATE_STOPPED;
                s_elapsed_ms = 0;
                s_total_ms = 0;
            }
            if (s_shutdown_requested) {
                break;
            }
            continue;
        }

        if (s_state == PLAYER_STATE_PLAYING) {
            if (!player_step_next(false)) {
                s_state = PLAYER_STATE_STOPPED;
                s_elapsed_ms = 0;
                s_total_ms = 0;
            }
        }
    }

    s_player_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t audio_player_init(const char *folder)
{
    if (!s_api_mutex) {
        s_api_mutex = xSemaphoreCreateMutex();
        if (!s_api_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    player_lock();
    if (folder && folder[0] != '\0') {
        strncpy(s_folder, folder, sizeof(s_folder) - 1);
        s_folder[sizeof(s_folder) - 1] = '\0';
    }

    if (!s_cmd_queue) {
        s_cmd_queue = xQueueCreate(PLAYER_QUEUE_DEPTH, sizeof(audio_player_cmd_t));
    }
    if (!s_cmd_queue) {
        player_unlock();
        return ESP_ERR_NO_MEM;
    }

    if (!s_player_task) {
        BaseType_t created = xTaskCreatePinnedToCore(player_task,
                                                     "audio_player",
                                                     8192,
                                                     NULL,
                                                     9,
                                                     &s_player_task,
                                                     PLAYER_DECODE_CORE);
        if (created != pdPASS) {
            s_player_task = NULL;
            player_unlock();
            return ESP_ERR_NO_MEM;
        }
    }

    s_shutdown_requested = false;
    player_scan_folder();
    player_unlock();
    return ESP_OK;
}

void audio_player_shutdown(void)
{
    player_lock();
    if (!s_cmd_queue && !s_player_task) {
        s_track_count = 0;
        s_state = PLAYER_STATE_STOPPED;
        s_request = REQ_NONE;
        s_elapsed_ms = 0;
        s_total_ms = 0;
        player_unlock();
        return;
    }

    s_shutdown_requested = true;
    s_state = PLAYER_STATE_STOPPED;
    s_request = REQ_STOP;
    if (s_cmd_queue) {
        audio_player_cmd_t cmd = {
            .type = CMD_SHUTDOWN
        };
        if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
            xQueueReset(s_cmd_queue);
            xQueueSend(s_cmd_queue, &cmd, 0);
        }
    }
    player_unlock();

    int retries = 50;
    while (s_player_task && retries-- > 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (s_player_task) {
        ESP_LOGW(TAG, "player shutdown timed out; task still running");
        return;
    }

    player_lock();
    if (s_cmd_queue) {
        xQueueReset(s_cmd_queue);
    }
    s_track_count = 0;
    s_order_index = 0;
    s_state = PLAYER_STATE_STOPPED;
    s_request = REQ_NONE;
    s_elapsed_ms = 0;
    s_total_ms = 0;
    s_shutdown_requested = false;
    player_unlock();
}

bool audio_player_is_ready(void)
{
    player_lock();
    bool ready = (s_cmd_queue != NULL && s_player_task != NULL);
    player_unlock();
    return ready;
}

void audio_player_rescan(void)
{
    player_lock();
    if (!s_cmd_queue) {
        player_unlock();
        return;
    }
    audio_player_cmd_t cmd = {
        .type = CMD_RESCAN
    };
    xQueueSend(s_cmd_queue, &cmd, 0);
    player_unlock();
}

void audio_player_set_volume(uint8_t volume)
{
    player_lock();
    s_volume = volume;
    player_unlock();
}

void audio_player_set_repeat_mode(audio_repeat_mode_t mode)
{
    player_lock();
    if (!s_cmd_queue) {
        s_repeat_mode = mode;
        player_build_order();
        player_unlock();
        return;
    }
    audio_player_cmd_t cmd = {
        .type = CMD_SET_REPEAT,
        .repeat_mode = mode
    };
    xQueueSend(s_cmd_queue, &cmd, 0);
    player_unlock();
}

audio_repeat_mode_t audio_player_get_repeat_mode(void)
{
    player_lock();
    audio_repeat_mode_t mode = s_repeat_mode;
    player_unlock();
    return mode;
}

audio_player_state_t audio_player_get_state(void)
{
    player_lock();
    audio_player_state_t state = s_state;
    player_unlock();
    return state;
}

uint16_t audio_player_get_track_index(void)
{
    player_lock();
    if (!player_has_tracks()) {
        player_unlock();
        return 0;
    }
    uint16_t index = (uint16_t)(s_order[s_order_index] + 1);
    player_unlock();
    return index;
}

uint16_t audio_player_get_track_count(void)
{
    player_lock();
    uint16_t count = s_track_count;
    player_unlock();
    return count;
}

void audio_player_get_time_ms(uint32_t *elapsed_ms, uint32_t *total_ms)
{
    player_lock();
    if (elapsed_ms) {
        *elapsed_ms = s_elapsed_ms;
    }
    if (total_ms) {
        *total_ms = s_total_ms;
    }
    player_unlock();
}

void audio_player_play(void)
{
    player_lock();
    if (!s_cmd_queue) {
        player_unlock();
        return;
    }
    audio_player_cmd_t cmd = {
        .type = CMD_PLAY
    };
    xQueueSend(s_cmd_queue, &cmd, 0);
    player_unlock();
}

void audio_player_pause(void)
{
    player_lock();
    if (!s_cmd_queue) {
        player_unlock();
        return;
    }
    audio_player_cmd_t cmd = {
        .type = CMD_PAUSE
    };
    xQueueSend(s_cmd_queue, &cmd, 0);
    player_unlock();
}

void audio_player_stop(void)
{
    player_lock();
    if (!s_cmd_queue) {
        s_state = PLAYER_STATE_STOPPED;
        s_request = REQ_NONE;
        s_elapsed_ms = 0;
        s_total_ms = 0;
        player_unlock();
        return;
    }
    audio_player_cmd_t cmd = {
        .type = CMD_STOP
    };
    xQueueSend(s_cmd_queue, &cmd, 0);
    player_unlock();
}

void audio_player_next(void)
{
    player_lock();
    if (!s_cmd_queue) {
        player_unlock();
        return;
    }
    audio_player_cmd_t cmd = {
        .type = CMD_NEXT
    };
    xQueueSend(s_cmd_queue, &cmd, 0);
    player_unlock();
}

void audio_player_prev(void)
{
    player_lock();
    if (!s_cmd_queue) {
        player_unlock();
        return;
    }
    audio_player_cmd_t cmd = {
        .type = CMD_PREV
    };
    xQueueSend(s_cmd_queue, &cmd, 0);
    player_unlock();
}

