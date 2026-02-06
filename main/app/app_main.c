#include "alarm_timer.h"
#include "alarm_actions.h"
#include "alarm_playback.h"
#include "app_control.h"
#include "alarm_sound.h"
#include "audio_pcm5102.h"
#include "audio_player.h"
#include "audio_eq.h"
#include "bt_avrc.h"
#include "bluetooth_sink.h"
#include "clock_time.h"
#include "config_store.h"
#include "config_owner.h"
#include "display_74hc595.h"
#include "display_ui.h"
#include "led_indicator.h"
#include "power_manager.h"
#include "ui_display_task.h"
#include "ui_input.h"
#include "ui_input_handlers.h"
#include "ui_mode_manager.h"
#include "ui_menu.h"
#include "storage_sd_spi.h"
#include "wifi_ntp.h"
#include "esp_system.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "esp_bt.h"

static app_config_t s_cfg;
#define DISPLAY_TEST_SEQUENCE 0

static uint8_t s_volume_level = 0;

static bool s_soft_off = false;
static uint8_t s_display_brightness = 255;
static bool s_alarm_active = false;

static void bt_volume_changed(uint8_t volume)
{
    ui_display_task_notify_bt_volume(volume);
}


static void alarm_event_handler(alarm_event_t event)
{
    if (event == ALARM_EVENT_TRIGGER || event == TIMER_EVENT_DONE) {
        s_alarm_active = true;
        alarm_actions_on_trigger();
        led_indicator_set_rgb(255, 0, 0);
        alarm_playback_start(&s_cfg);
        display_ui_show_text("ALRM", 1500);
        if (event == ALARM_EVENT_TRIGGER && s_cfg.alarm_mode == ALARM_MODE_ONCE) {
            s_cfg.alarm_enabled = false;
            config_owner_request_update(&s_cfg);
            alarm_set(s_cfg.alarm_hour, s_cfg.alarm_min, s_cfg.alarm_enabled,
                      (alarm_mode_t)s_cfg.alarm_mode);
        }
    }
}



void app_main(void)
{
    esp_log_level_set("coexist", ESP_LOG_ERROR);
    esp_log_level_set("gpio", ESP_LOG_WARN);
    esp_log_level_set("BTDM_INIT", ESP_LOG_ERROR);
    esp_log_level_set("BT_HCI", ESP_LOG_ERROR);
    esp_log_level_set("BT_APPL", ESP_LOG_ERROR);
    esp_log_level_set("BT_BTC", ESP_LOG_ERROR);
    esp_log_level_set("BT_LOG", ESP_LOG_ERROR);
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(config_store_init());
    config_store_get(&s_cfg);
    if (s_cfg.volume > APP_VOLUME_MAX) {
        s_cfg.volume = app_volume_steps_from_byte(s_cfg.volume);
    }
    s_volume_level = s_cfg.volume;
    s_display_brightness = s_cfg.display_brightness;
    config_owner_init(&s_cfg);
    config_owner_start();
    ui_menu_init(&s_cfg, &s_display_brightness);
    ui_display_task_init(&s_cfg, &s_volume_level, &s_soft_off);
    ui_input_handlers_init(&s_cfg, &s_volume_level, &s_display_brightness, &s_soft_off, &s_alarm_active);
    ui_mode_manager_init(&s_cfg, &s_display_brightness, &s_soft_off);
    bt_avrc_register_volume_cb(bt_volume_changed);

    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);

    clock_time_init(s_cfg.tz);
    power_manager_init();
    power_manager_set_autonomous(s_cfg.power_save_enabled);
    power_manager_handle_boot();
    display_init();
#if DISPLAY_TEST_SEQUENCE
    // Temporary test: cycle digits/letters and brightness once per second.
    static const char test_chars[] = "0123456789AbCDEFGHIJLNOPRSTUVYX";
    size_t idx = 0;
    uint8_t brightness = 0;
    int8_t step = 32;
    while (1) {
        char text[5];
        char c = test_chars[idx];
        text[0] = c;
        text[1] = c;
        text[2] = c;
        text[3] = c;
        text[4] = '\0';
        display_set_text(text, false);
        display_set_brightness(brightness);
        idx = (idx + 1) % (sizeof(test_chars) - 1);
        int next = (int)brightness + step;
        if (next >= 255) {
            brightness = 255;
            step = -step;
        } else if (next <= 0) {
            brightness = 0;
            step = -step;
        } else {
            brightness = (uint8_t)next;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif
    display_ui_init();
    display_set_brightness(s_display_brightness);
    display_set_static(true);
    led_indicator_init();
    audio_init();
    alarm_playback_init();
    audio_eq_set_steps(s_cfg.eq_low, s_cfg.eq_high);
    audio_set_volume(app_volume_steps_to_byte(s_cfg.volume));
    storage_sd_init();
    audio_player_set_volume(app_volume_steps_to_byte(s_cfg.volume));
    bt_avrc_notify_volume(app_volume_steps_to_byte(s_cfg.volume));
    wifi_init(&s_cfg);
    ui_input_init(ui_input_handle_encoder, ui_input_handle_adc_key);

    ui_mode_manager_start();
    app_set_ui_mode(APP_UI_MODE_CLOCK);
    alarm_timer_init(alarm_event_handler);
    alarm_set(s_cfg.alarm_hour, s_cfg.alarm_min, s_cfg.alarm_enabled,
              (alarm_mode_t)s_cfg.alarm_mode);

    ui_display_task_start();
}
