#include "ui_input_handlers.h"

#include "app_control.h"
#include "alarm_actions.h"
#include "alarm_playback.h"
#include "audio_pcm5102.h"
#include "audio_player.h"
#include "bt_avrc.h"
#include "bluetooth_sink.h"
#include "display_74hc595.h"
#include "display_ui.h"
#include "led_indicator.h"
#include "radio_rda5807.h"
#include "ui_display_task.h"
#include "ui_menu.h"
#include "ui_time_setting.h"
#include "wifi_ntp.h"

static app_config_t *s_cfg = NULL;
static uint8_t *s_volume_level = NULL;
static uint8_t *s_display_brightness = NULL;
static bool *s_soft_off = NULL;
static bool *s_alarm_active = NULL;
static const uint8_t DISPLAY_DIM_LEVEL = 10;

static void alarm_acknowledge(void)
{
    if (!s_alarm_active || !*s_alarm_active) {
        return;
    }
    *s_alarm_active = false;
    alarm_playback_stop();
    alarm_actions_on_ack();
    led_indicator_set_rgb(0, 0, 0);
}

static void soft_power_apply(bool off)
{
    if (!s_soft_off || !s_display_brightness) {
        return;
    }
    if (off == *s_soft_off) {
        return;
    }
    *s_soft_off = off;

    if (*s_soft_off) {
        audio_player_stop();
        audio_stop();
        radio_rda5807_set_muted(true);
        radio_rda5807_set_enabled(false);
        wifi_set_enabled(false);
        led_indicator_set_rgb(0, 25, 0);
        display_set_brightness(DISPLAY_DIM_LEVEL);
        ui_time_setting_reset();
        ui_menu_exit();
        return;
    }

    led_indicator_set_rgb(0, 0, 0);
    display_set_brightness(*s_display_brightness);
    wifi_set_enabled(true);
    if (app_get_ui_mode() == APP_UI_MODE_RADIO) {
        radio_rda5807_set_enabled(true);
        radio_rda5807_set_muted(false);
    }
}

static void ui_mode_cycle(void)
{
    app_ui_mode_t mode = app_get_ui_mode();
    if (mode == APP_UI_MODE_CLOCK) {
        app_request_ui_mode(APP_UI_MODE_PLAYER);
    } else if (mode == APP_UI_MODE_PLAYER) {
        app_request_ui_mode(APP_UI_MODE_BLUETOOTH);
    } else if (mode == APP_UI_MODE_BLUETOOTH) {
        app_request_ui_mode(APP_UI_MODE_RADIO);
    } else {
        app_request_ui_mode(APP_UI_MODE_CLOCK);
    }
}

void ui_input_handlers_init(app_config_t *cfg,
                            uint8_t *volume_level,
                            uint8_t *display_brightness,
                            bool *soft_off,
                            bool *alarm_active)
{
    s_cfg = cfg;
    s_volume_level = volume_level;
    s_display_brightness = display_brightness;
    s_soft_off = soft_off;
    s_alarm_active = alarm_active;
}

void ui_input_handle_encoder(encoder_event_t event)
{
    bool bt_connected = bt_avrc_is_connected();

    if (s_alarm_active && *s_alarm_active) {
        alarm_acknowledge();
        return;
    }
    if (app_ui_is_busy()) {
        return;
    }

    app_ui_mode_t menu_mode = APP_UI_MODE_CLOCK;
    ui_menu_action_t menu_action = ui_menu_handle_encoder(event, &menu_mode);
    if (menu_action == UI_MENU_ACTION_ENTER_TIME_SETTING) {
        ui_display_task_clear_overlay();
        ui_time_setting_enter();
        return;
    }
    if (menu_action == UI_MENU_ACTION_SET_MODE) {
        app_request_ui_mode(menu_mode);
        return;
    }
    if (menu_action == UI_MENU_ACTION_HANDLED) {
        return;
    }

    if (event == ENC_EVENT_BTN_LONG) {
        if (!ui_time_setting_is_active()) {
            ui_display_task_clear_overlay();
            ui_menu_enter();
        }
        return;
    }

    if (event == ENC_EVENT_BTN_SHORT) {
        if (ui_time_setting_handle_short_press()) {
            return;
        }

        if (app_get_ui_mode() == APP_UI_MODE_BLUETOOTH && bt_connected) {
            if (bt_sink_is_playing()) {
                bt_avrc_send_command(BT_AVRC_CMD_PAUSE);
                display_ui_show_text("PAUS", 1000);
            } else {
                bt_avrc_send_command(BT_AVRC_CMD_PLAY);
                display_ui_show_text("PLAY", 1000);
            }
        } else if (app_get_ui_mode() == APP_UI_MODE_PLAYER) {
            if (audio_player_get_state() == PLAYER_STATE_PLAYING) {
                audio_player_pause();
                display_ui_show_text("PAUS", 1000);
            } else {
                audio_player_play();
                ui_display_task_show_track_overlay(audio_player_get_track_index(),
                                                   audio_player_get_track_count());
            }
        }
        return;
    }

    if (event == ENC_EVENT_CW || event == ENC_EVENT_CCW) {
        int delta = (event == ENC_EVENT_CW) ? 1 : -1;

        if (ui_time_setting_handle_knob(delta)) {
            return;
        }

        if (!s_volume_level || !s_cfg) {
            return;
        }
        if (delta > 0 && *s_volume_level < APP_VOLUME_MAX) {
            (*s_volume_level)++;
        } else if (delta < 0 && *s_volume_level > 0) {
            (*s_volume_level)--;
        }
        uint8_t scaled = app_volume_steps_to_byte(*s_volume_level);
        audio_set_volume(scaled);
        audio_player_set_volume(scaled);
        radio_rda5807_set_volume_steps(*s_volume_level);
        bt_avrc_notify_volume(scaled);
        ui_display_task_mark_volume_dirty();
        ui_display_task_show_volume(*s_volume_level);
    }
}

void ui_input_handle_adc_key(adc_key_id_t key, adc_key_event_t event)
{
    if (s_alarm_active && *s_alarm_active) {
        alarm_acknowledge();
        return;
    }

    if (app_ui_is_busy()) {
        if (key == ADC_KEY_POWER && event == ADC_KEY_EVENT_SHORT) {
            if (s_soft_off) {
                soft_power_apply(!*s_soft_off);
            }
        }
        return;
    }

    if (key == ADC_KEY_POWER && event == ADC_KEY_EVENT_SHORT) {
        if (s_soft_off) {
            soft_power_apply(!*s_soft_off);
        }
        return;
    }

    if (s_soft_off && *s_soft_off) {
        return;
    }

    if (key == ADC_KEY_MODE) {
        if (event == ADC_KEY_EVENT_LONG) {
            if (ui_menu_is_active()) {
                ui_menu_exit();
            } else {
                ui_display_task_clear_overlay();
                ui_menu_enter();
            }
        } else if (event == ADC_KEY_EVENT_SHORT) {
            ui_mode_cycle();
        }
        return;
    }

    if ((key == ADC_KEY_NEXT || key == ADC_KEY_PREV) && event == ADC_KEY_EVENT_SHORT) {
        if (app_get_ui_mode() == APP_UI_MODE_BLUETOOTH) {
            if (bt_avrc_is_connected()) {
                bt_avrc_send_command(key == ADC_KEY_NEXT ? BT_AVRC_CMD_NEXT : BT_AVRC_CMD_PREV);
            }
        } else if (app_get_ui_mode() == APP_UI_MODE_PLAYER) {
            if (audio_player_get_state() != PLAYER_STATE_STOPPED) {
                if (key == ADC_KEY_NEXT) {
                    audio_player_next();
                } else {
                    audio_player_prev();
                }
            }
        } else if (app_get_ui_mode() == APP_UI_MODE_RADIO) {
            radio_rda5807_step(key == ADC_KEY_NEXT);
        }
        return;
    }

    if (key == ADC_KEY_BT && event == ADC_KEY_EVENT_LONG) {
        if (app_get_ui_mode() != APP_UI_MODE_BLUETOOTH) {
            return;
        }
        if (bt_sink_is_connected() || bt_avrc_is_connected()) {
            bt_sink_disconnect();
            bt_sink_set_discoverable(true);
            display_ui_show_text("BLUE", 1200);
        } else {
            bt_sink_clear_bonds();
            bt_sink_set_discoverable(true);
            display_ui_show_text("CLr ", 1200);
        }
        return;
    }
}
