/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOSConfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "audio_owner.h"
#include "audio_pcm5102.h"
#include "audio_spectrum.h"
#include "bt_app_core.h"

#define RINGBUF_HIGHEST_WATER_LEVEL    (64 * 1024)
#define RINGBUF_PREFETCH_START_BYTES   (40 * 1024)
#define RINGBUF_RESUME_WATER_LEVEL     (24 * 1024)
#define RINGBUF_PREFETCH_PACKET_COUNT  12
#define RINGBUF_MIN_WATER_LEVEL        (24 * 1024)
#define BT_I2S_CHUNK_BYTES             (240 * 6)
#define BT_I2S_WRITE_TIMEOUT_MS        50

enum {
    RINGBUFFER_MODE_PROCESSING,    /* ringbuffer is buffering incoming audio data, I2S is working */
    RINGBUFFER_MODE_PREFETCHING,   /* ringbuffer is buffering incoming audio data, I2S is waiting */
    RINGBUFFER_MODE_DROPPING       /* ringbuffer is not buffering (dropping) incoming audio data, I2S is working */
};

/*******************************
 * STATIC FUNCTION DECLARATIONS
 ******************************/

/* handler for application task */
static void bt_app_task_handler(void *arg);
/* handler for I2S task */
static void bt_i2s_task_handler(void *arg);
/* message sender */
static bool bt_app_send_msg(bt_app_msg_t *msg);
/* handle dispatched messages */
static void bt_app_work_dispatched(bt_app_msg_t *msg);
/* ringbuffer init */
static bool bt_ringbuf_ensure_init(void);
/* ringbuffer reset (must hold s_ringbuf_mutex) */
static void bt_ringbuf_reset_locked(void);
/* ringbuffer size selector */
static size_t bt_ringbuf_select_size(size_t max_bytes);

/*******************************
 * STATIC VARIABLE DEFINITIONS
 ******************************/

static QueueHandle_t s_bt_app_task_queue = NULL;  /* handle of work queue */
static TaskHandle_t s_bt_app_task_handle = NULL;  /* handle of application task  */
static TaskHandle_t s_bt_i2s_task_handle = NULL;  /* handle of I2S task */
static SemaphoreHandle_t s_i2s_write_semaphore = NULL;
static uint16_t ringbuffer_mode = RINGBUFFER_MODE_PROCESSING;

static uint8_t *s_ringbuf_storage = NULL;
static size_t s_ringbuf_size = 0;
static size_t s_prefetch_start_bytes = 0;
static size_t s_resume_water_level = 0;
static size_t s_prefetch_packet_target = 0;
static size_t s_ringbuf_head = 0;
static size_t s_ringbuf_tail = 0;
static size_t s_ringbuf_count = 0;
static size_t s_prefetch_packets = 0;
static uint32_t s_ringbuf_gen = 0;
static bool s_ringbuf_enabled = false;
static SemaphoreHandle_t s_ringbuf_mutex = NULL;
static uint32_t s_bt_error_count = 0;
static bool s_bt_mute_active = false;
static volatile bool s_bt_i2s_stop_requested = false;
static uint8_t s_silence_chunk[BT_I2S_CHUNK_BYTES] = {0};

/*******************************
 * STATIC FUNCTION DEFINITIONS
 ******************************/

static size_t bt_ringbuf_select_size(size_t max_bytes)
{
    static const size_t k_sizes[] = {
        64 * 1024,
        48 * 1024,
        32 * 1024,
        24 * 1024
    };
    for (size_t i = 0; i < (sizeof(k_sizes) / sizeof(k_sizes[0])); ++i) {
        if (k_sizes[i] <= max_bytes) {
            return k_sizes[i];
        }
    }
    return 0;
}

static bool bt_ringbuf_ensure_init(void)
{
    size_t max_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (!s_ringbuf_enabled) {
        return false;
    }

    if (!s_ringbuf_mutex) {
        s_ringbuf_mutex = xSemaphoreCreateMutex();
        if (!s_ringbuf_mutex) {
            ESP_LOGE(BT_APP_CORE_TAG, "%s, ringbuffer mutex create failed", __func__);
            return false;
        }
    }

    if (xSemaphoreTake(s_ringbuf_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    if (!s_ringbuf_storage) {
        size_t candidate = bt_ringbuf_select_size(max_internal);
        if (candidate) {
            s_ringbuf_storage = heap_caps_malloc(candidate, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (s_ringbuf_storage) {
                s_ringbuf_size = candidate;
            }
        }
        if (!s_ringbuf_storage) {
            xSemaphoreGive(s_ringbuf_mutex);
            ESP_LOGE(BT_APP_CORE_TAG, "%s, ringbuffer alloc failed (max internal=%u)",
                     __func__, (unsigned)max_internal);
            return false;
        }

        s_prefetch_start_bytes = RINGBUF_PREFETCH_START_BYTES;
        if (s_prefetch_start_bytes > (s_ringbuf_size * 3 / 4)) {
            s_prefetch_start_bytes = s_ringbuf_size * 3 / 4;
        }
        if (s_prefetch_start_bytes < (BT_I2S_CHUNK_BYTES * 4)) {
            s_prefetch_start_bytes = BT_I2S_CHUNK_BYTES * 4;
        }

        s_resume_water_level = RINGBUF_RESUME_WATER_LEVEL;
        if (s_resume_water_level > (s_ringbuf_size / 2)) {
            s_resume_water_level = s_ringbuf_size / 2;
        }
        if (s_resume_water_level < BT_I2S_CHUNK_BYTES) {
            s_resume_water_level = BT_I2S_CHUNK_BYTES;
        }

        if (s_resume_water_level >= s_prefetch_start_bytes) {
            s_resume_water_level = s_prefetch_start_bytes / 2;
            if (s_resume_water_level < BT_I2S_CHUNK_BYTES) {
                s_resume_water_level = BT_I2S_CHUNK_BYTES;
            }
        }

        s_prefetch_packet_target = RINGBUF_PREFETCH_PACKET_COUNT;
        if (s_ringbuf_size < 24 * 1024) {
            s_prefetch_packet_target = 4;
        } else if (s_ringbuf_size < 32 * 1024) {
            s_prefetch_packet_target = 6;
        } else if (s_ringbuf_size < 48 * 1024) {
            s_prefetch_packet_target = 8;
        }

    }

    xSemaphoreGive(s_ringbuf_mutex);
    return true;
}

static void bt_ringbuf_reset_locked(void)
{
    s_ringbuf_head = 0;
    s_ringbuf_tail = 0;
    s_ringbuf_count = 0;
    s_prefetch_packets = 0;
    ringbuffer_mode = RINGBUFFER_MODE_PREFETCHING;
    s_ringbuf_gen++;
}

bool bt_app_core_reserve_ringbuffer(size_t size)
{
    if (size < RINGBUF_MIN_WATER_LEVEL) {
        size = RINGBUF_MIN_WATER_LEVEL;
    }

    s_ringbuf_enabled = true;
    if (!s_ringbuf_mutex) {
        s_ringbuf_mutex = xSemaphoreCreateMutex();
        if (!s_ringbuf_mutex) {
            ESP_LOGE(BT_APP_CORE_TAG, "%s, ringbuffer mutex create failed", __func__);
            return false;
        }
    }

    if (xSemaphoreTake(s_ringbuf_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    if (s_ringbuf_storage) {
        xSemaphoreGive(s_ringbuf_mutex);
        return true;
    }

    size_t max_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (max_internal < RINGBUF_MIN_WATER_LEVEL) {
        xSemaphoreGive(s_ringbuf_mutex);
        ESP_LOGW(BT_APP_CORE_TAG, "ringbuffer reserve failed: max_internal=%u",
                 (unsigned)max_internal);
        return false;
    }
    size_t limit = (size < max_internal) ? size : max_internal;
    size_t selected = bt_ringbuf_select_size(limit);
    if (!selected) {
        xSemaphoreGive(s_ringbuf_mutex);
        ESP_LOGW(BT_APP_CORE_TAG, "ringbuffer reserve failed: max_internal=%u requested=%u",
                 (unsigned)max_internal, (unsigned)size);
        return false;
    }

    uint8_t *buf = heap_caps_malloc(selected, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        xSemaphoreGive(s_ringbuf_mutex);
        ESP_LOGW(BT_APP_CORE_TAG, "ringbuffer reserve alloc failed: size=%u", (unsigned)selected);
        return false;
    }

    s_ringbuf_storage = buf;
    s_ringbuf_size = selected;
    s_prefetch_start_bytes = RINGBUF_PREFETCH_START_BYTES;
    if (s_prefetch_start_bytes > (s_ringbuf_size * 3 / 4)) {
        s_prefetch_start_bytes = s_ringbuf_size * 3 / 4;
    }
    if (s_prefetch_start_bytes < (BT_I2S_CHUNK_BYTES * 4)) {
        s_prefetch_start_bytes = BT_I2S_CHUNK_BYTES * 4;
    }

    s_resume_water_level = RINGBUF_RESUME_WATER_LEVEL;
    if (s_resume_water_level > (s_ringbuf_size / 2)) {
        s_resume_water_level = s_ringbuf_size / 2;
    }
    if (s_resume_water_level < BT_I2S_CHUNK_BYTES) {
        s_resume_water_level = BT_I2S_CHUNK_BYTES;
    }

    if (s_resume_water_level >= s_prefetch_start_bytes) {
        s_resume_water_level = s_prefetch_start_bytes / 2;
        if (s_resume_water_level < BT_I2S_CHUNK_BYTES) {
            s_resume_water_level = BT_I2S_CHUNK_BYTES;
        }
    }

    s_prefetch_packet_target = RINGBUF_PREFETCH_PACKET_COUNT;
    if (s_ringbuf_size < 24 * 1024) {
        s_prefetch_packet_target = 4;
    } else if (s_ringbuf_size < 32 * 1024) {
        s_prefetch_packet_target = 6;
    } else if (s_ringbuf_size < 48 * 1024) {
        s_prefetch_packet_target = 8;
    }

    xSemaphoreGive(s_ringbuf_mutex);
    return true;
}

void bt_app_core_release_ringbuffer(void)
{
    s_ringbuf_enabled = false;
    if (s_ringbuf_mutex && xSemaphoreTake(s_ringbuf_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_ringbuf_storage) {
            free(s_ringbuf_storage);
            s_ringbuf_storage = NULL;
        }
        s_ringbuf_size = 0;
        bt_ringbuf_reset_locked();
        xSemaphoreGive(s_ringbuf_mutex);
        return;
    }
    if (s_ringbuf_storage) {
        free(s_ringbuf_storage);
        s_ringbuf_storage = NULL;
    }
    s_ringbuf_size = 0;
    s_ringbuf_head = 0;
    s_ringbuf_tail = 0;
    s_ringbuf_count = 0;
    s_prefetch_packets = 0;
    ringbuffer_mode = RINGBUFFER_MODE_PREFETCHING;
    s_ringbuf_gen++;
}

void bt_app_core_reset_ringbuffer(void)
{
    if (s_ringbuf_mutex && xSemaphoreTake(s_ringbuf_mutex, portMAX_DELAY) == pdTRUE) {
        bt_ringbuf_reset_locked();
        xSemaphoreGive(s_ringbuf_mutex);
    }
}

static inline void bt_app_core_inc_error(void)
{
    (void)__atomic_fetch_add(&s_bt_error_count, 1, __ATOMIC_RELAXED);
}

static void bt_app_core_set_mute(bool enable)
{
    s_bt_mute_active = enable;
}

static bool bt_app_send_msg(bt_app_msg_t *msg)
{
    if (msg == NULL) {
        return false;
    }
    if (!s_bt_app_task_queue) {
        return false;
    }

    /* send the message to work queue */
    if (xQueueSend(s_bt_app_task_queue, msg, 10 / portTICK_PERIOD_MS) != pdTRUE) {
        UBaseType_t waiting = s_bt_app_task_queue ? uxQueueMessagesWaiting(s_bt_app_task_queue) : 0;
        UBaseType_t spaces = s_bt_app_task_queue ? uxQueueSpacesAvailable(s_bt_app_task_queue) : 0;
        eTaskState state = s_bt_app_task_handle ? eTaskGetState(s_bt_app_task_handle) : eInvalid;
        ESP_LOGE(BT_APP_CORE_TAG,
                 "%s xQueue send failed (waiting=%u spaces=%u task=%p state=%d)",
                 __func__, (unsigned)waiting, (unsigned)spaces, (void *)s_bt_app_task_handle, (int)state);
        return false;
    }
    return true;
}

static void bt_app_work_dispatched(bt_app_msg_t *msg)
{
    if (msg->cb) {
        msg->cb(msg->event, msg->param);
    }
}

static void bt_app_task_handler(void *arg)
{
    bt_app_msg_t msg;

    for (;;) {
        /* receive message from work queue and handle it */
        if (pdTRUE == xQueueReceive(s_bt_app_task_queue, &msg, (TickType_t)portMAX_DELAY)) {
            switch (msg.sig) {
            case BT_APP_SIG_WORK_DISPATCH:
                bt_app_work_dispatched(&msg);
                break;
            default:
                ESP_LOGW(BT_APP_CORE_TAG, "%s, unhandled signal: %d", __func__, msg.sig);
                break;
            }

            if (msg.param) {
                free(msg.param);
            }
        }
    }
}

static void bt_i2s_task_handler(void *arg)
{
    size_t bytes_written = 0;

    for (;;) {
        if (s_bt_i2s_stop_requested) {
            break;
        }
        if (pdTRUE == xSemaphoreTake(s_i2s_write_semaphore, portMAX_DELAY)) {
            if (s_bt_i2s_stop_requested) {
                break;
            }
            if (audio_owner_get() != AUDIO_OWNER_BT) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            for (;;) {
                if (s_bt_i2s_stop_requested) {
                    break;
                }
                uint8_t *data = NULL;
                size_t item_size = 0;
                uint32_t local_gen = 0;
                bool prefetching = false;

                if (s_ringbuf_mutex && xSemaphoreTake(s_ringbuf_mutex, portMAX_DELAY) == pdTRUE) {
                    prefetching = (ringbuffer_mode == RINGBUFFER_MODE_PREFETCHING);
                    if (!prefetching && s_ringbuf_count > 0) {
                        size_t contiguous = s_ringbuf_size - s_ringbuf_tail;
                        size_t available = (s_ringbuf_count < contiguous) ? s_ringbuf_count : contiguous;
                        if (available > BT_I2S_CHUNK_BYTES) {
                            available = BT_I2S_CHUNK_BYTES;
                        }
                        data = &s_ringbuf_storage[s_ringbuf_tail];
                        item_size = available;
                        local_gen = s_ringbuf_gen;
                    }
                    xSemaphoreGive(s_ringbuf_mutex);
                }

                if (item_size == 0) {
                    bool mode_prefetch_changed = false;
                    if (s_ringbuf_mutex && xSemaphoreTake(s_ringbuf_mutex, portMAX_DELAY) == pdTRUE) {
                        if (ringbuffer_mode != RINGBUFFER_MODE_PREFETCHING) {
                            ringbuffer_mode = RINGBUFFER_MODE_PREFETCHING;
                            s_prefetch_packets = 0;
                            mode_prefetch_changed = true;
                        }
                        xSemaphoreGive(s_ringbuf_mutex);
                    }
                    if (mode_prefetch_changed) {
                        bt_app_core_inc_error();
                    }
                    if (!s_bt_mute_active) {
                        bt_app_core_set_mute(true);
                    }
                    size_t silence_written = 0;
                    audio_i2s_write(s_silence_chunk, sizeof(s_silence_chunk), &silence_written, BT_I2S_WRITE_TIMEOUT_MS);
                    vTaskDelay(pdMS_TO_TICKS(2));
                    continue;
                }

                if (s_bt_mute_active) {
                    memset(data, 0, item_size);
                }

                if (!s_bt_mute_active && item_size >= sizeof(int16_t)) {
                    size_t sample_count = item_size / sizeof(int16_t);
#if AUDIO_SPECTRUM_ENABLE
                    audio_spectrum_feed((const int16_t *)data, sample_count, 2);
#endif
                }

                uint8_t vol = audio_get_volume();
                if (vol < 255 && item_size >= sizeof(int16_t)) {
                    int16_t *samples = (int16_t *)data;
                    size_t count = item_size / sizeof(int16_t);
                    for (size_t i = 0; i < count; ++i) {
                        samples[i] = (int16_t)(((int32_t)samples[i] * vol) / 255);
                    }
                }

                bytes_written = 0;
                esp_err_t err = audio_i2s_write(data, item_size, &bytes_written, BT_I2S_WRITE_TIMEOUT_MS);
                if (err != ESP_OK || bytes_written == 0) {
                    bt_app_core_inc_error();
                    audio_i2s_reset();
                    ESP_LOGE(BT_APP_CORE_TAG, "i2s write failed: %s (%d), bytes=%u",
                             esp_err_to_name(err), err, (unsigned)bytes_written);
                    if (s_ringbuf_mutex && xSemaphoreTake(s_ringbuf_mutex, portMAX_DELAY) == pdTRUE) {
                        bt_ringbuf_reset_locked();
                        xSemaphoreGive(s_ringbuf_mutex);
                    }
                    if (!s_bt_mute_active) {
                        bt_app_core_set_mute(true);
                    }
                    vTaskDelay(pdMS_TO_TICKS(10));
                    break;
                }

                bool log_mode_processing = false;
                if (s_ringbuf_mutex && xSemaphoreTake(s_ringbuf_mutex, portMAX_DELAY) == pdTRUE) {
                    if (local_gen != s_ringbuf_gen) {
                        xSemaphoreGive(s_ringbuf_mutex);
                        continue;
                    }
                    size_t consumed = (bytes_written <= item_size) ? bytes_written : item_size;
                    s_ringbuf_tail = (s_ringbuf_tail + consumed) % s_ringbuf_size;
                    s_ringbuf_count -= consumed;
                    if (ringbuffer_mode == RINGBUFFER_MODE_DROPPING && s_ringbuf_count <= s_resume_water_level) {
                        ringbuffer_mode = RINGBUFFER_MODE_PROCESSING;
                        log_mode_processing = true;
                    }
                    xSemaphoreGive(s_ringbuf_mutex);
                }
                if (log_mode_processing) {
                    if (s_bt_mute_active) {
                        bt_app_core_set_mute(false);
                    }
                }
            }
        }
    }

    s_bt_i2s_task_handle = NULL;
    s_bt_i2s_stop_requested = false;
    vTaskDelete(NULL);
}

/********************************
 * EXTERNAL FUNCTION DEFINITIONS
 *******************************/

bool bt_app_work_dispatch(bt_app_cb_t p_cback, uint16_t event, void *p_params, int param_len, bt_app_copy_cb_t p_copy_cback)
{
    bt_app_msg_t msg;
    memset(&msg, 0, sizeof(bt_app_msg_t));

    msg.sig = BT_APP_SIG_WORK_DISPATCH;
    msg.event = event;
    msg.cb = p_cback;

    if (param_len == 0) {
        if (!bt_app_send_msg(&msg)) {
            ESP_LOGW(BT_APP_CORE_TAG, "dispatch failed (no param) evt=0x%x", event);
            return false;
        }
        return true;
    } else if (p_params && param_len > 0) {
        if ((msg.param = malloc(param_len)) != NULL) {
            memcpy(msg.param, p_params, param_len);
            /* check if caller has provided a copy callback to do the deep copy */
            if (p_copy_cback) {
                p_copy_cback(msg.param, p_params, param_len);
            }
            if (!bt_app_send_msg(&msg)) {
                ESP_LOGW(BT_APP_CORE_TAG, "dispatch failed (queue) evt=0x%x len=%d", event, param_len);
                free(msg.param);
                return false;
            }
            return true;
        }
        ESP_LOGW(BT_APP_CORE_TAG, "dispatch failed (malloc) evt=0x%x len=%d", event, param_len);
    }

    return false;
}

void bt_app_task_start_up(void)
{
    if (!s_bt_app_task_queue) {
        s_bt_app_task_queue = xQueueCreate(20, sizeof(bt_app_msg_t));
        if (!s_bt_app_task_queue) {
            ESP_LOGE(BT_APP_CORE_TAG, "bt app queue create failed");
        }
    }
    if (!s_bt_app_task_handle) {
        BaseType_t created = xTaskCreate(bt_app_task_handler, "BtAppTask", 4096, NULL, 10, &s_bt_app_task_handle);
        if (created != pdPASS) {
            ESP_LOGE(BT_APP_CORE_TAG, "BtAppTask create failed");
            s_bt_app_task_handle = NULL;
        }
    }
}

void bt_app_task_shut_down(void)
{
    if (s_bt_app_task_handle) {
        vTaskDelete(s_bt_app_task_handle);
        s_bt_app_task_handle = NULL;
    }
    if (s_bt_app_task_queue) {
        vQueueDelete(s_bt_app_task_queue);
        s_bt_app_task_queue = NULL;
    }
}

bool bt_i2s_task_is_running(void)
{
    return s_bt_i2s_task_handle != NULL;
}

void bt_i2s_task_start_up(void)
{
    if (!audio_owner_acquire(AUDIO_OWNER_BT, false)) {
        ESP_LOGW(BT_APP_CORE_TAG, "BtI2STask start skipped (audio owner busy)");
        return;
    }
    audio_spectrum_reset();
    s_bt_i2s_stop_requested = false;
    if (!bt_ringbuf_ensure_init()) {
        ESP_LOGW(BT_APP_CORE_TAG, "BtI2STask start skipped (ringbuffer init failed)");
        audio_owner_release(AUDIO_OWNER_BT);
        return;
    }
    audio_i2s_reset();
    if (!s_i2s_write_semaphore) {
        s_i2s_write_semaphore = xSemaphoreCreateBinary();
    }
    if (!s_i2s_write_semaphore) {
        ESP_LOGE(BT_APP_CORE_TAG, "%s, semaphore create failed", __func__);
        audio_owner_release(AUDIO_OWNER_BT);
        return;
    }
    if (xSemaphoreTake(s_ringbuf_mutex, portMAX_DELAY) == pdTRUE) {
        ringbuffer_mode = RINGBUFFER_MODE_PREFETCHING;
        s_prefetch_packets = 0;
        s_ringbuf_head = 0;
        s_ringbuf_tail = 0;
        s_ringbuf_count = 0;
        xSemaphoreGive(s_ringbuf_mutex);
    }

    if (!s_bt_i2s_task_handle) {
        BaseType_t created = xTaskCreate(bt_i2s_task_handler,
                                         "BtI2STask",
                                         4096,
                                         NULL,
                                         configMAX_PRIORITIES - 3,
                                         &s_bt_i2s_task_handle);
        if (created != pdPASS) {
            ESP_LOGE(BT_APP_CORE_TAG, "BtI2STask create failed");
            s_bt_i2s_task_handle = NULL;
            audio_owner_release(AUDIO_OWNER_BT);
            return;
        }
    }

    if (s_i2s_write_semaphore) {
        xSemaphoreGive(s_i2s_write_semaphore);
    }
}

void bt_i2s_task_shut_down(void)
{
    audio_spectrum_reset();
    if (s_bt_i2s_task_handle) {
        s_bt_i2s_stop_requested = true;
        if (s_i2s_write_semaphore) {
            xSemaphoreGive(s_i2s_write_semaphore);
        }
        for (int i = 0; i < 50 && s_bt_i2s_task_handle; ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (s_bt_i2s_task_handle) {
            ESP_LOGW(BT_APP_CORE_TAG, "BtI2STask stop timeout, leaving task running");
        }
    }
    audio_i2s_write_silence(50);
    if (!s_bt_i2s_task_handle && s_i2s_write_semaphore) {
        vSemaphoreDelete(s_i2s_write_semaphore);
        s_i2s_write_semaphore = NULL;
    }
    audio_owner_release(AUDIO_OWNER_BT);
}

size_t write_ringbuf(const uint8_t *data, size_t size)
{
    if (!data || size == 0) {
        return 0;
    }

    if (!bt_ringbuf_ensure_init()) {
        return 0;
    }

    bool log_mode_processing = false;
    bool log_mode_processing_prefetch = false;
    bool give_i2s_semaphore = false;

    if (xSemaphoreTake(s_ringbuf_mutex, portMAX_DELAY) != pdTRUE) {
        return 0;
    }
    if (!s_ringbuf_storage || s_ringbuf_size == 0) {
        xSemaphoreGive(s_ringbuf_mutex);
        return 0;
    }
    if (s_ringbuf_head >= s_ringbuf_size || s_ringbuf_tail >= s_ringbuf_size || s_ringbuf_count > s_ringbuf_size) {
        s_ringbuf_head = 0;
        s_ringbuf_tail = 0;
        s_ringbuf_count = 0;
        s_prefetch_packets = 0;
        ringbuffer_mode = RINGBUFFER_MODE_PREFETCHING;
        xSemaphoreGive(s_ringbuf_mutex);
        bt_app_core_inc_error();
        ESP_LOGE(BT_APP_CORE_TAG, "ringbuffer state invalid, reset");
        return 0;
    }
    if (ringbuffer_mode == RINGBUFFER_MODE_DROPPING) {
        if (s_ringbuf_count <= s_resume_water_level) {
            ringbuffer_mode = RINGBUFFER_MODE_PROCESSING;
            log_mode_processing = true;
        } else {
            bt_ringbuf_reset_locked();
            xSemaphoreGive(s_ringbuf_mutex);
            bt_app_core_inc_error();
            if (!s_bt_mute_active) {
                bt_app_core_set_mute(true);
            }
            ESP_LOGD(BT_APP_CORE_TAG, "ringbuffer overflowed, reset buffer");
            return 0;
        }
    }

    size_t free_space = s_ringbuf_size - s_ringbuf_count;
    if (free_space == 0) {
        bt_ringbuf_reset_locked();
        xSemaphoreGive(s_ringbuf_mutex);
        bt_app_core_inc_error();
        if (!s_bt_mute_active) {
            bt_app_core_set_mute(true);
        }
        ESP_LOGD(BT_APP_CORE_TAG, "ringbuffer overflowed, reset buffer");
        return 0;
    }

    size_t to_write = (size <= free_space) ? size : free_space;
    size_t first = s_ringbuf_size - s_ringbuf_head;
    if (first > to_write) {
        first = to_write;
    }
    memcpy(&s_ringbuf_storage[s_ringbuf_head], data, first);
    s_ringbuf_head = (s_ringbuf_head + first) % s_ringbuf_size;
    if (to_write > first) {
        size_t second = to_write - first;
        memcpy(&s_ringbuf_storage[s_ringbuf_head], data + first, second);
        s_ringbuf_head = (s_ringbuf_head + second) % s_ringbuf_size;
    }
    s_ringbuf_count += to_write;

    if (ringbuffer_mode == RINGBUFFER_MODE_PREFETCHING && to_write > 0) {
        s_prefetch_packets++;
        bool reached_bytes = s_ringbuf_count >= s_prefetch_start_bytes;
        bool reached_packets = s_prefetch_packets >= s_prefetch_packet_target;
        if (reached_bytes || reached_packets) {
            ringbuffer_mode = RINGBUFFER_MODE_PROCESSING;
            s_prefetch_packets = 0;
            log_mode_processing_prefetch = true;
            give_i2s_semaphore = true;
        }
    }

    xSemaphoreGive(s_ringbuf_mutex);

    if ((log_mode_processing || log_mode_processing_prefetch) && s_bt_mute_active) {
        bt_app_core_set_mute(false);
    }
    if (give_i2s_semaphore && s_i2s_write_semaphore) {
        if (pdFALSE == xSemaphoreGive(s_i2s_write_semaphore)) {
            if (s_bt_i2s_task_handle && !s_bt_i2s_stop_requested) {
                eTaskState state = eTaskGetState(s_bt_i2s_task_handle);
                if (state != eDeleted) {
                    UBaseType_t watermark = uxTaskGetStackHighWaterMark(s_bt_i2s_task_handle);
                    ESP_LOGD(BT_APP_CORE_TAG,
                             "semaphore give failed (i2s_task=%p state=%d hwm=%u mode=%u count=%u)",
                             (void *)s_bt_i2s_task_handle,
                             (int)state,
                             (unsigned)watermark,
                             (unsigned)ringbuffer_mode,
                             (unsigned)s_ringbuf_count);
                }
            }
        }
    }

    if (to_write < size) {
        bt_app_core_inc_error();
    }
    return to_write;
}

uint32_t bt_app_core_get_error_count(void)
{
    return __atomic_load_n(&s_bt_error_count, __ATOMIC_RELAXED);
}

size_t bt_app_core_get_ringbuffer_size(void)
{
    size_t size = 0;
    if (s_ringbuf_mutex && xSemaphoreTake(s_ringbuf_mutex, portMAX_DELAY) == pdTRUE) {
        size = s_ringbuf_size;
        xSemaphoreGive(s_ringbuf_mutex);
        return size;
    }
    return s_ringbuf_size;
}
