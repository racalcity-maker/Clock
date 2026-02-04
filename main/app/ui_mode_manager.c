#include "ui_mode_manager.h"

#include "app_control.h"
#include "audio_pcm5102.h"
#include "audio_spectrum.h"
#include "audio_player.h"
#include "bt_app_core.h"
#include "bt_avrc.h"
#include "bluetooth_sink.h"
#include "display_74hc595.h"
#include "display_bt_anim.h"
#include "display_ui.h"
#include "led_indicator.h"
#include "storage_sd_spi.h"
#include "ui_display_task.h"
#include "web_config.h"
#include "wifi_ntp.h"
#include "power_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

// Temporary test mode: cycles UI modes every 10 seconds.
#define UI_MODE_TEST_CYCLE 0
#define UI_MODE_HEAP_LOG 0
#define UI_MODE_TEST_INTERVAL_MS 15000
#define UI_MODE_HEAP_LOG_INTERVAL_MS (5 * 60 * 1000)

static const char *TAG = "ui_mode";

static app_config_t *s_cfg = NULL;
static uint8_t *s_display_brightness = NULL;
static bool *s_soft_off = NULL;

typedef enum {
    UI_MODE_CLOCK,
    UI_MODE_PLAYER,
    UI_MODE_BLUETOOTH
} ui_mode_t;

static ui_mode_t s_ui_mode = UI_MODE_CLOCK;
static bool s_ui_mode_initialized = false;
static QueueHandle_t s_ui_cmd_queue = NULL;
static TaskHandle_t s_ui_cmd_task = NULL;
#if UI_MODE_HEAP_LOG
static TaskHandle_t s_heap_log_task = NULL;
#endif
static bool s_web_stop_pending = false;
static volatile int64_t s_ui_busy_until_us = 0;
static volatile bool s_ui_busy_force = false;
static int64_t s_next_heap_log_us = 0;
#if UI_MODE_TEST_CYCLE
static TaskHandle_t s_ui_test_task = NULL;
#endif
static encoder_event_cb_t s_encoder_cb = NULL;
static adc_key_event_cb_t s_adc_cb = NULL;

typedef enum {
    UI_CMD_SET_MODE,
    UI_CMD_INPUT_ENC,
    UI_CMD_INPUT_ADC
} ui_cmd_type_t;

typedef struct {
    ui_cmd_type_t type;
    app_ui_mode_t mode;
    union {
        encoder_event_t enc;
        struct {
            adc_key_id_t key;
            adc_key_event_t event;
        } adc;
    } data;
} ui_cmd_t;

static void ui_cmd_task(void *arg);
#if UI_MODE_HEAP_LOG
static void heap_log_task(void *arg);
#endif
#if UI_MODE_TEST_CYCLE
static void ui_test_cycle_task(void *arg);
#endif
static void web_config_stop_deferred(void);
static void enter_clock_mode(void);
static void enter_player_mode(void);
static void enter_bluetooth_mode(void);
static void stop_player_and_flush(void);
static void wait_for_heap_release(uint32_t settle_ms, uint32_t timeout_ms);
static void ui_mode_log_heap(const char *tag);

static void ui_mode_log_heap(const char *tag)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t bt_rb = bt_app_core_get_ringbuffer_size();
    ESP_LOGI(TAG, "heap %s: free=%u internal=%u largest_int=%u bt_rb=%u",
             tag ? tag : "-",
             (unsigned)info.total_free_bytes,
             (unsigned)info.total_free_bytes,
             (unsigned)info.largest_free_block,
             (unsigned)bt_rb);
}

static void ui_cmd_start(void)
{
    if (s_ui_cmd_queue) {
        return;
    }
    s_ui_cmd_queue = xQueueCreate(16, sizeof(ui_cmd_t));
    if (s_ui_cmd_queue) {
        if (xTaskCreate(ui_cmd_task, "ui_cmd", 4096, NULL, 6, &s_ui_cmd_task) != pdPASS) {
            vQueueDelete(s_ui_cmd_queue);
            s_ui_cmd_queue = NULL;
            s_ui_cmd_task = NULL;
            ESP_LOGW(TAG, "ui cmd task create failed");
        }
    } else {
        ESP_LOGW(TAG, "ui cmd queue create failed");
    }
}

#if UI_MODE_HEAP_LOG
static void heap_log_start(void)
{
    if (s_heap_log_task) {
        return;
    }
    if (xTaskCreate(heap_log_task, "heap_log", 2048, NULL, 1, &s_heap_log_task) != pdPASS) {
        s_heap_log_task = NULL;
        ESP_LOGW(TAG, "heap log task create failed");
    }
}

static void heap_log_stop(void)
{
    if (s_heap_log_task) {
        vTaskDelete(s_heap_log_task);
        s_heap_log_task = NULL;
    }
}
#endif

static void web_config_stop_task(void *arg)
{
    (void)arg;
    web_config_stop();
    s_web_stop_pending = false;
    vTaskDelete(NULL);
}

static void web_config_stop_deferred(void)
{
    if (s_web_stop_pending) {
        return;
    }
    s_web_stop_pending = true;
    if (xTaskCreate(web_config_stop_task, "web_cfg_stop", 2048, NULL, 5, NULL) != pdPASS) {
        s_web_stop_pending = false;
        ESP_LOGW(TAG, "web config stop task create failed");
    }
}

static void wait_for_heap_release(uint32_t settle_ms, uint32_t timeout_ms)
{
    int64_t start_us = esp_timer_get_time();
    size_t last_block = 0;
    int64_t stable_since_us = 0;

    while ((esp_timer_get_time() - start_us) < ((int64_t)timeout_ms * 1000)) {
        size_t now_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (now_block != last_block) {
            last_block = now_block;
            stable_since_us = esp_timer_get_time();
        }
        if (!s_web_stop_pending && stable_since_us != 0 &&
            (esp_timer_get_time() - stable_since_us) >= ((int64_t)settle_ms * 1000)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void ui_mode_manager_init(app_config_t *cfg, uint8_t *display_brightness, bool *soft_off)
{
    s_cfg = cfg;
    s_display_brightness = display_brightness;
    s_soft_off = soft_off;
}

void ui_mode_manager_start(void)
{
    ui_cmd_start();
    s_next_heap_log_us = esp_timer_get_time() + ((int64_t)UI_MODE_HEAP_LOG_INTERVAL_MS * 1000);
#if UI_MODE_HEAP_LOG
    heap_log_start();
#endif
#if UI_MODE_TEST_CYCLE
    if (!s_ui_test_task) {
        xTaskCreate(ui_test_cycle_task, "ui_mode_test", 2048, NULL, 2, &s_ui_test_task);
    }
#endif
}

app_ui_mode_t app_get_ui_mode(void)
{
    if (s_ui_mode == UI_MODE_PLAYER) {
        return APP_UI_MODE_PLAYER;
    }
    if (s_ui_mode == UI_MODE_BLUETOOTH) {
        return APP_UI_MODE_BLUETOOTH;
    }
    return APP_UI_MODE_CLOCK;
}

bool app_ui_is_busy(void)
{
    int64_t now = esp_timer_get_time();
    bool forced = __atomic_load_n(&s_ui_busy_force, __ATOMIC_ACQUIRE);
    int64_t until = __atomic_load_n(&s_ui_busy_until_us, __ATOMIC_ACQUIRE);
    if (!forced && until != 0 && now >= until) {
        __atomic_store_n(&s_ui_busy_until_us, 0, __ATOMIC_RELEASE);
        return false;
    }
    return forced || (until != 0 && now < until);
}

void app_ui_set_busy(bool busy)
{
    __atomic_store_n(&s_ui_busy_force, busy, __ATOMIC_RELEASE);
    if (!busy) {
        __atomic_store_n(&s_ui_busy_until_us, 0, __ATOMIC_RELEASE);
    }
}

void app_ui_busy_for_ms(uint32_t duration_ms)
{
    int64_t now = esp_timer_get_time();
    int64_t until = now + (int64_t)duration_ms * 1000;
    int64_t prev = __atomic_load_n(&s_ui_busy_until_us, __ATOMIC_ACQUIRE);
    if (until > prev) {
        __atomic_store_n(&s_ui_busy_until_us, until, __ATOMIC_RELEASE);
    }
}

static void ui_cmd_task(void *arg)
{
    (void)arg;
    ui_cmd_t cmd;
    while (1) {
        if (xQueueReceive(s_ui_cmd_queue, &cmd, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (cmd.type == UI_CMD_SET_MODE) {
                app_set_ui_mode(cmd.mode);
            } else if (cmd.type == UI_CMD_INPUT_ENC) {
                if (s_encoder_cb) {
                    s_encoder_cb(cmd.data.enc);
                }
            } else if (cmd.type == UI_CMD_INPUT_ADC) {
                if (s_adc_cb) {
                    s_adc_cb(cmd.data.adc.key, cmd.data.adc.event);
                }
            }
        }

        int64_t now = esp_timer_get_time();
        if (s_next_heap_log_us == 0 || now >= s_next_heap_log_us) {
            ui_mode_log_heap("periodic");
            s_next_heap_log_us = now + ((int64_t)UI_MODE_HEAP_LOG_INTERVAL_MS * 1000);
        }
    }
}

void app_request_ui_mode(app_ui_mode_t mode)
{
    if (!s_ui_cmd_queue) {
        app_set_ui_mode(mode);
        return;
    }
    ui_cmd_t cmd = {
        .type = UI_CMD_SET_MODE,
        .mode = mode
    };
    if (xQueueSend(s_ui_cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "ui cmd queue full");
        app_set_ui_mode(mode);
    }
}

void ui_mode_manager_set_input_handlers(encoder_event_cb_t encoder_cb, adc_key_event_cb_t adc_cb)
{
    s_encoder_cb = encoder_cb;
    s_adc_cb = adc_cb;
}

void app_request_input_encoder(encoder_event_t event)
{
    if (!s_ui_cmd_queue) {
        if (s_encoder_cb) {
            s_encoder_cb(event);
        }
        return;
    }
    ui_cmd_t cmd = {
        .type = UI_CMD_INPUT_ENC
    };
    cmd.data.enc = event;
    if (xQueueSend(s_ui_cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "ui cmd queue full (enc)");
    }
}

void app_request_input_adc(adc_key_id_t key, adc_key_event_t event)
{
    if (!s_ui_cmd_queue) {
        if (s_adc_cb) {
            s_adc_cb(key, event);
        }
        return;
    }
    ui_cmd_t cmd = {
        .type = UI_CMD_INPUT_ADC
    };
    cmd.data.adc.key = key;
    cmd.data.adc.event = event;
    if (xQueueSend(s_ui_cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "ui cmd queue full (adc)");
    }
}

void app_set_ui_mode(app_ui_mode_t mode)
{
    ui_mode_t new_mode = UI_MODE_CLOCK;
    if (mode == APP_UI_MODE_PLAYER) {
        new_mode = UI_MODE_PLAYER;
    } else if (mode == APP_UI_MODE_BLUETOOTH) {
        new_mode = UI_MODE_BLUETOOTH;
    }
    if (s_ui_mode_initialized && s_ui_mode == new_mode) {
        return;
    }
    app_ui_set_busy(true);
    s_ui_mode = new_mode;
    s_ui_mode_initialized = true;
    ui_display_task_set_overlays_enabled(true);
    const char *label = "CLCK";
    if (s_ui_mode == UI_MODE_PLAYER) {
        label = "PLYR";
        enter_player_mode();
        led_indicator_set_seconds_rgb(255, 60, 0);
    } else if (s_ui_mode == UI_MODE_BLUETOOTH) {
        label = "BLUE";
        enter_bluetooth_mode();
        led_indicator_set_seconds_rgb(0, 160, 255);
    } else {
        enter_clock_mode();
        led_indicator_set_seconds_rgb(255, 0, 0);
    }
    app_ui_set_busy(false);
    app_ui_busy_for_ms(800);
    ESP_LOGI(TAG, "ui mode set: %s", label);
    ui_mode_log_heap("mode_switch");
    display_ui_show_text(label, 800);
}

static void enter_clock_mode(void)
{
    display_pause_refresh(true);
    stop_player_and_flush();
    if (bt_sink_is_ready()) {
        bt_sink_set_discoverable(false);
        if (bt_sink_is_connected() || bt_avrc_is_connected()) {
            bt_sink_disconnect();
        }
    }
    bt_i2s_task_shut_down();
    bt_app_core_release_ringbuffer();
    bt_sink_deinit();
    audio_spectrum_enable(false);
    if (!storage_sd_is_mounted()) {
        storage_sd_init();
    }
    if (!audio_player_is_ready()) {
        audio_player_init("/sdcard/music");
    }
    display_pause_refresh(false);
}

static void enter_player_mode(void)
{
    display_pause_refresh(true);
    if (bt_sink_is_ready()) {
        if (bt_sink_is_connected() || bt_avrc_is_connected()) {
            bt_sink_disconnect();
        }
        bt_sink_set_discoverable(false);
    }
    bt_i2s_task_shut_down();
    bt_app_core_release_ringbuffer();
    bt_sink_deinit();
    audio_spectrum_enable(false);
    if (wifi_is_enabled()) {
        esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        if (ps_err != ESP_OK) {
            ESP_LOGW(TAG, "wifi ps set failed: %s", esp_err_to_name(ps_err));
        }
    }
    if (!storage_sd_is_mounted()) {
        storage_sd_init();
    }
    if (!audio_player_is_ready()) {
        audio_player_init("/sdcard/music");
    }
    audio_player_rescan();
    display_pause_refresh(false);
}

static void enter_bluetooth_mode(void)
{
    display_pause_refresh(true);
    display_set_text("    ", false);
    ESP_LOGD(TAG, "bt switch: stop tasks: display_task, power_monitor");
    ui_display_task_pause();
    power_manager_pause();
    stop_player_and_flush();
#if UI_MODE_HEAP_LOG
    heap_log_stop();
#endif
    if (wifi_is_enabled()) {
        wifi_set_enabled(false);
    }
    web_config_stop_deferred();
    wifi_wait_for_shutdown(1500);
    if (storage_sd_is_mounted()) {
        storage_sd_unmount();
    }
    wait_for_heap_release(100, 1500);
    bt_app_core_reserve_ringbuffer(64 * 1024);
    display_bt_anim_reset(esp_timer_get_time());
    ui_display_task_clear_overlay();
    if (!bt_sink_is_ready()) {
        esp_err_t err = bt_sink_init(s_cfg ? s_cfg->bt_name : NULL);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "bt init failed: %s", esp_err_to_name(err));
        }
    }
    if (bt_sink_is_ready()) {
        audio_spectrum_enable(true);
    } else {
        audio_spectrum_enable(false);
    }
    ESP_LOGD(TAG, "bt switch: start tasks: power_monitor, display_task");
    power_manager_resume();
    ui_display_task_resume();
    if (s_display_brightness) {
        display_set_brightness(*s_display_brightness);
    }
#if UI_MODE_HEAP_LOG
    heap_log_start();
#endif
    display_pause_refresh(false);
    bt_sink_set_discoverable(true);
    ESP_LOGD(TAG, "bt autoconnect disabled; waiting for source");
}

static void stop_player_and_flush(void)
{
    if (!audio_player_is_ready()) {
        return;
    }
    audio_player_stop();
    for (int i = 0; i < 200; ++i) {
        if (audio_player_get_state() == PLAYER_STATE_STOPPED) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    audio_player_shutdown();
    audio_i2s_write_silence(120);
    audio_i2s_reset();
}

#if UI_MODE_HEAP_LOG
static void heap_log_task(void *arg)
{
    (void)arg;
    while (1) {
        multi_heap_info_t info;
        heap_caps_get_info(&info, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t bt_rb = bt_app_core_get_ringbuffer_size();
        ESP_LOGI(TAG, "heap: free=%u internal=%u largest_int=%u bt_rb=%u",
                 (unsigned)info.total_free_bytes,
                 (unsigned)info.total_free_bytes,
                 (unsigned)info.largest_free_block,
                 (unsigned)bt_rb);
        vTaskDelay(pdMS_TO_TICKS(UI_MODE_HEAP_LOG_INTERVAL_MS));
    }
}
#endif

void ui_mode_manager_heap_snapshot(const char *tag)
{
    ui_mode_log_heap(tag);
}

#if UI_MODE_TEST_CYCLE
static void ui_test_cycle_task(void *arg)
{
    (void)arg;
    const app_ui_mode_t modes[] = {
        APP_UI_MODE_CLOCK,
        APP_UI_MODE_PLAYER,
        APP_UI_MODE_BLUETOOTH
    };
    size_t idx = 0;
    vTaskDelay(pdMS_TO_TICKS(UI_MODE_TEST_INTERVAL_MS));
    while (1) {
        app_request_ui_mode(modes[idx]);
        idx = (idx + 1) % (sizeof(modes) / sizeof(modes[0]));
        vTaskDelay(pdMS_TO_TICKS(UI_MODE_TEST_INTERVAL_MS));
    }
}
#endif
