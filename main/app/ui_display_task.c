#include "ui_display_task.h"

#include "app_control.h"
#include "alarm_actions.h"
#include "audio_pcm5102.h"
#include "audio_player.h"
#include "bluetooth_sink.h"
#include "clock_time.h"
#include "config_store.h"
#include "config_owner.h"
#include "display_74hc595.h"
#include "display_bt_anim.h"
#include "display_ui.h"
#include "storage_sd_spi.h"
#include "ui_menu.h"
#include "ui_time_setting.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <time.h>


static const uint32_t TRACK_OVERLAY_MS = 10000;
static const uint32_t TRACK_NUMBER_MS = 5000;
static const uint32_t TRACK_REMAIN_UPDATE_MS = 1000;
static const uint32_t TRACK_OVERLAY_PERIOD_MS = 60000;
static const uint32_t VOLUME_OVERLAY_MS = 800;

typedef enum {
    TRACK_OVERLAY_NONE,
    TRACK_OVERLAY_NUMBER,
    TRACK_OVERLAY_REMAIN,
    TRACK_OVERLAY_BLUE
} track_overlay_state_t;

static app_config_t *s_cfg = NULL;
static uint8_t *s_volume_level = NULL;
static bool *s_soft_off = NULL;

static track_overlay_state_t s_overlay_state = TRACK_OVERLAY_NONE;
static int64_t s_overlay_stage_until_us = 0;
static int64_t s_overlay_end_us = 0;
static int64_t s_next_remain_update_us = 0;
static int64_t s_next_overlay_us = 0;
static bool s_last_music_playing = false;

static volatile uint8_t s_bt_vol_pending = 0;
static volatile bool s_bt_vol_pending_valid = false;
static bool s_task_started = false;
static TaskHandle_t s_display_task_handle = NULL;
static app_ui_mode_t s_last_mode = APP_UI_MODE_CLOCK;
static bool s_post_mode_pending = false;
static int64_t s_post_mode_due_us = 0;
static bool s_volume_dirty = false;
static int64_t s_volume_last_change_us = 0;
static const int64_t VOLUME_SAVE_IDLE_US = 60000000;
static volatile bool s_overlays_enabled = true;
static uint8_t s_last_hours = 0;
static uint8_t s_last_minutes = 0;
static bool s_last_colon = false;

static void schedule_player_status(int64_t now_us)
{
    s_post_mode_pending = true;
    s_post_mode_due_us = now_us + 900000;
}

static void show_player_status(void)
{
    if (!s_overlays_enabled) {
        return;
    }
    if (!storage_sd_is_mounted()) {
        display_ui_show_text("SdEr", 1200);
        return;
    }

    uint16_t count = audio_player_get_track_count();
    if (count == 0) {
        display_ui_show_text("NOFL", 1200);
        return;
    }

    if (count > 9999) {
        count = 9999;
    }
    char text[5];
    snprintf(text, sizeof(text), "%4u", (unsigned)count);
    display_ui_show_text(text, 1200);
}

static void show_track_number(uint16_t track_index, uint16_t track_count, uint32_t duration_ms)
{
    if (!s_overlays_enabled) {
        return;
    }
    char text[5] = "tr--";
    if (track_count > 0 && track_index > 0) {
        uint16_t shown = track_index;
        if (shown > 99) {
            shown %= 100;
        }
        snprintf(text, sizeof(text), "tr%02u", (unsigned)shown);
    }
    display_ui_show_text(text, duration_ms);
}

static void show_remaining_time(uint32_t remaining_ms, uint32_t duration_ms)
{
    if (!s_overlays_enabled) {
        return;
    }
    uint32_t seconds = remaining_ms / 1000;
    uint32_t minutes = seconds / 60;
    seconds %= 60;

    if (minutes > 99) {
        minutes = 99;
    }

    uint8_t digits[4] = {
        (uint8_t)(minutes / 10),
        (uint8_t)(minutes % 10),
        (uint8_t)(seconds / 10),
        (uint8_t)(seconds % 10)
    };
    display_ui_show_digits(digits, true, duration_ms);
}

static void show_volume_level(uint8_t volume)
{
    if (!s_overlays_enabled) {
        return;
    }
    char text[5];
    if (volume > APP_VOLUME_MAX) {
        volume = APP_VOLUME_MAX;
    }
    snprintf(text, sizeof(text), "V%03u", (unsigned)volume);
    display_ui_show_text(text, VOLUME_OVERLAY_MS);
}

void ui_display_task_mark_volume_dirty(void)
{
    s_volume_dirty = true;
    s_volume_last_change_us = esp_timer_get_time();
}

void ui_display_task_set_overlays_enabled(bool enabled)
{
    s_overlays_enabled = enabled;
    if (!enabled) {
        ui_display_task_clear_overlay();
        display_ui_show_text(NULL, 0);
    }
}

static void start_track_overlay(uint16_t track_index, uint16_t track_count)
{
    if (!s_overlays_enabled) {
        return;
    }
    int64_t now_us = esp_timer_get_time();
    s_overlay_state = TRACK_OVERLAY_NUMBER;
    s_overlay_stage_until_us = now_us + (int64_t)TRACK_NUMBER_MS * 1000;
    s_overlay_end_us = now_us + (int64_t)TRACK_OVERLAY_MS * 1000;
    s_next_remain_update_us = now_us;
    s_next_overlay_us = now_us + (int64_t)TRACK_OVERLAY_PERIOD_MS * 1000;
    show_track_number(track_index, track_count, TRACK_NUMBER_MS);
}

static void start_bt_overlay(void)
{
    if (!s_overlays_enabled) {
        return;
    }
    int64_t now_us = esp_timer_get_time();
    s_overlay_state = TRACK_OVERLAY_BLUE;
    s_overlay_stage_until_us = 0;
    s_overlay_end_us = now_us + (int64_t)TRACK_NUMBER_MS * 1000;
    s_next_remain_update_us = 0;
    s_next_overlay_us = now_us + (int64_t)TRACK_OVERLAY_PERIOD_MS * 1000;
    display_ui_show_text("BLUE", TRACK_NUMBER_MS);
}

static bool music_is_playing(app_ui_mode_t mode)
{
    if (mode == APP_UI_MODE_BLUETOOTH) {
        return bt_sink_is_streaming();
    }
    return (audio_player_get_state() == PLAYER_STATE_PLAYING);
}

static void render_clock_or_placeholder(void)
{
    if (clock_time_is_valid()) {
        display_set_time(s_last_hours, s_last_minutes, s_last_colon);
    } else {
        display_set_text("----", true);
    }
}

static void display_task(void *arg)
{
    (void)arg;
    bool colon = false;
    int tick = 0;
    uint16_t last_track = 0;
    while (1) {
        alarm_actions_poll();
        if (__atomic_exchange_n(&s_bt_vol_pending_valid, false, __ATOMIC_ACQ_REL)) {
            uint8_t bt_vol = __atomic_load_n(&s_bt_vol_pending, __ATOMIC_RELAXED);
            uint8_t steps = app_volume_steps_from_byte(bt_vol);
            if (s_volume_level && s_cfg && steps != *s_volume_level) {
                *s_volume_level = steps;
                uint8_t scaled = app_volume_steps_to_byte(steps);
                audio_set_volume(scaled);
                audio_player_set_volume(scaled);
                show_volume_level(steps);
                ui_display_task_mark_volume_dirty();
            }
        }

        if (s_volume_dirty && s_cfg && s_volume_level) {
            int64_t now_us = esp_timer_get_time();
            if (now_us - s_volume_last_change_us >= VOLUME_SAVE_IDLE_US) {
                app_config_t cfg_copy = *s_cfg;
                cfg_copy.volume = *s_volume_level;
                config_owner_request_update(&cfg_copy);
                s_volume_dirty = false;
            }
        }

        if (ui_menu_is_active()) {
            ui_menu_render();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (s_soft_off && *s_soft_off) {
            if (tick == 0) {
                if (clock_time_is_valid()) {
                    struct tm now;
                    clock_time_get(&now);
                    colon = !colon;
                    s_last_hours = (uint8_t)now.tm_hour;
                    s_last_minutes = (uint8_t)now.tm_min;
                    s_last_colon = colon;
                } else {
                    s_last_hours = 0xFF;
                    s_last_minutes = 0xFF;
                    s_last_colon = true;
                }
                render_clock_or_placeholder();
            }
            tick = (tick + 1) % 10;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (ui_time_setting_is_active()) {
            if (ui_time_setting_should_exit()) {
                ui_time_setting_reset();
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            ui_time_setting_render();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (tick == 0) {
            if (clock_time_is_valid()) {
                struct tm now;
                clock_time_get(&now);
                colon = !colon;
                s_last_hours = (uint8_t)now.tm_hour;
                s_last_minutes = (uint8_t)now.tm_min;
                s_last_colon = colon;
            } else {
                s_last_hours = 0xFF;
                s_last_minutes = 0xFF;
                s_last_colon = true;
            }
            display_ui_set_time(s_last_hours, s_last_minutes, s_last_colon);
        }

        app_ui_mode_t mode = app_get_ui_mode();
        if (mode != s_last_mode) {
            s_last_mode = mode;
            if (mode == APP_UI_MODE_PLAYER) {
                schedule_player_status(esp_timer_get_time());
            } else {
                s_post_mode_pending = false;
            }
        }
        bool music_playing = music_is_playing(mode);
        bool bt_streaming = (mode == APP_UI_MODE_BLUETOOTH) ? bt_sink_is_streaming() : false;
        bool audio_playing = (mode == APP_UI_MODE_PLAYER) ? (audio_player_get_state() == PLAYER_STATE_PLAYING)
                                                          : false;

        if (mode == APP_UI_MODE_BLUETOOTH) {
            int64_t now_us = esp_timer_get_time();
            if (s_overlays_enabled && bt_streaming) {
                if (!s_last_music_playing) {
                    display_bt_anim_reset(now_us);
                }
                display_bt_anim_update(now_us);
            } else if (display_ui_overlay_active() && display_ui_overlay_is_segments()) {
                // Clear leftover BT animation segments so the clock stays visible.
                display_ui_show_text(NULL, 0);
            }
            if (!s_overlays_enabled) {
                if (display_ui_overlay_active()) {
                    display_ui_show_text(NULL, 0);
                }
                render_clock_or_placeholder();
            } else {
                display_ui_render();
            }
            tick = (tick + 1) % 5;
            vTaskDelay(pdMS_TO_TICKS(100));
            s_last_music_playing = music_playing;
            continue;
        }

        if (s_overlays_enabled && mode == APP_UI_MODE_PLAYER && s_post_mode_pending) {
            int64_t now_us = esp_timer_get_time();
            if (now_us >= s_post_mode_due_us && !display_ui_overlay_active()) {
                show_player_status();
                s_post_mode_pending = false;
            }
        }
        if (s_overlays_enabled && music_playing && !s_last_music_playing) {
            s_next_overlay_us = esp_timer_get_time() + (int64_t)TRACK_OVERLAY_PERIOD_MS * 1000;
        }
        if (!music_playing) {
            last_track = 0;
            s_overlay_state = TRACK_OVERLAY_NONE;
            s_next_overlay_us = 0;
        }

        if (s_overlays_enabled && audio_playing) {
            uint16_t track = audio_player_get_track_index();
            if (track != 0 && track != last_track) {
                start_track_overlay(track, audio_player_get_track_count());
            }
            last_track = track;
        } else {
            last_track = 0;
        }

        if (s_overlays_enabled && music_playing && s_overlay_state == TRACK_OVERLAY_NONE && s_next_overlay_us != 0) {
            int64_t now_us = esp_timer_get_time();
            if (now_us >= s_next_overlay_us) {
                if (audio_playing) {
                    start_track_overlay(audio_player_get_track_index(),
                                        audio_player_get_track_count());
                } else if (bt_streaming) {
                    start_bt_overlay();
                }
            }
        }

        if (s_overlays_enabled && s_overlay_state != TRACK_OVERLAY_NONE) {
            int64_t now_us = esp_timer_get_time();
            if (now_us >= s_overlay_end_us) {
                s_overlay_state = TRACK_OVERLAY_NONE;
            } else if (s_overlay_state == TRACK_OVERLAY_NUMBER && now_us >= s_overlay_stage_until_us) {
                s_overlay_state = TRACK_OVERLAY_REMAIN;
                s_next_remain_update_us = 0;
            }
            if (s_overlay_state == TRACK_OVERLAY_REMAIN && now_us >= s_next_remain_update_us) {
                uint32_t elapsed_ms = 0;
                uint32_t total_ms = 0;
                if (audio_playing) {
                    audio_player_get_time_ms(&elapsed_ms, &total_ms);
                }
                uint32_t remaining_ms = 0;
                if (total_ms > elapsed_ms) {
                    remaining_ms = total_ms - elapsed_ms;
                }
                show_remaining_time(remaining_ms, TRACK_REMAIN_UPDATE_MS + 100);
                s_next_remain_update_us = now_us + (int64_t)TRACK_REMAIN_UPDATE_MS * 1000;
            }
        }

        display_ui_render();
        tick = (tick + 1) % 5;
        vTaskDelay(pdMS_TO_TICKS(100));

        s_last_music_playing = music_playing;
    }
}

void ui_display_task_init(app_config_t *cfg, uint8_t *volume_level, bool *soft_off)
{
    s_cfg = cfg;
    s_volume_level = volume_level;
    s_soft_off = soft_off;
}

void ui_display_task_start(void)
{
    if (s_task_started) {
        return;
    }
    if (xTaskCreate(display_task, "display_task", 3072, NULL, 5, &s_display_task_handle) == pdPASS) {
        s_task_started = true;
    } else {
        s_display_task_handle = NULL;
        s_task_started = false;
    }
}

void ui_display_task_pause(void)
{
    if (s_display_task_handle) {
        vTaskDelete(s_display_task_handle);
        s_display_task_handle = NULL;
        s_task_started = false;
    }
}

void ui_display_task_resume(void)
{
    ui_display_task_start();
}

void ui_display_task_notify_bt_volume(uint8_t volume)
{
    __atomic_store_n(&s_bt_vol_pending, volume, __ATOMIC_RELAXED);
    __atomic_store_n(&s_bt_vol_pending_valid, true, __ATOMIC_RELEASE);
}

void ui_display_task_show_volume(uint8_t volume)
{
    show_volume_level(volume);
}

void ui_display_task_clear_overlay(void)
{
    s_overlay_state = TRACK_OVERLAY_NONE;
    s_overlay_stage_until_us = 0;
    s_overlay_end_us = 0;
    s_next_remain_update_us = 0;
    s_next_overlay_us = 0;
}

void ui_display_task_show_track_overlay(uint16_t track_index, uint16_t track_count)
{
    start_track_overlay(track_index, track_count);
}
