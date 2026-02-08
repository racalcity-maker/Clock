#include "ui_menu.h"

#include "alarm_timer.h"
#include "app_control.h"
#include "alarm_sound.h"
#include "audio_eq.h"
#include "audio_pcm5102.h"
#include "audio_player.h"
#include "bt_avrc.h"
#include "bluetooth_sink.h"
#include "config_store.h"
#include "config_owner.h"
#include "display_74hc595.h"
#include "power_manager.h"
#include "storage_sd_spi.h"
#include "wifi_ntp.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

static const uint8_t BRIGHTNESS_STEP = 5;
static const int64_t MENU_TIMEOUT_US = 10000000; // 10s
static const int64_t BRIGHTNESS_SAVE_IDLE_US = 2000000; // 2s
static const int64_t EQ_SAVE_IDLE_US = 2000000; // 2s
static const int64_t ALARM_TONE_PREVIEW_DELAY_US = 250000; // 250ms
static const uint32_t ALARM_TONE_PREVIEW_MS = 0;
static const int64_t MENU_FORBIDDEN_US = 1200000; // 1.2s

typedef enum {
    MENU_ROOT_CLOCK,
    MENU_ROOT_PLAYER,
    MENU_ROOT_BLUETOOTH,
    MENU_ROOT_RADIO,
    MENU_ROOT_EQ,
    MENU_ROOT_SETTINGS,
    MENU_ROOT_COUNT
} menu_root_item_t;

typedef enum {
    MENU_SET_TIME,
    MENU_SET_ALARM_MENU,
    MENU_SET_BRIGHTNESS,
    MENU_SET_WEB,
    MENU_SET_POWER_SAVE,
    MENU_SET_COUNT
} menu_set_item_t;

typedef enum {
    MENU_ALARM_ENABLE,
    MENU_ALARM_TIME,
    MENU_ALARM_VOLUME,
    MENU_ALARM_TONE,
    MENU_ALARM_REPEAT,
    MENU_ALARM_MODE,
    MENU_ALARM_COUNT
} menu_alarm_item_t;

typedef enum {
    MENU_STATE_ROOT,
    MENU_STATE_SETTINGS_LIST,
    MENU_STATE_ALARM_LIST,
    MENU_STATE_BRIGHTNESS,
    MENU_STATE_EQ,
    MENU_STATE_ALARM_TIME,
    MENU_STATE_ALARM_MODE,
    MENU_STATE_ALARM_TONE,
    MENU_STATE_ALARM_VOLUME,
    MENU_STATE_ALARM_REPEAT
} menu_state_t;

static app_config_t *s_cfg = NULL;
static uint8_t *s_display_brightness = NULL;
static menu_root_item_t s_menu_root_item = MENU_ROOT_CLOCK;
static menu_set_item_t s_menu_set_item = MENU_SET_TIME;
static menu_alarm_item_t s_menu_alarm_item = MENU_ALARM_ENABLE;
static menu_state_t s_menu_state = MENU_STATE_ROOT;
static bool s_menu_active = false;
static int64_t s_menu_last_activity_us = 0;
static bool s_brightness_dirty = false;
static int64_t s_brightness_last_change_us = 0;
static bool s_eq_dirty = false;
static int64_t s_eq_last_change_us = 0;
static uint8_t s_eq_select = 0;
static app_ui_mode_t s_alarm_tone_resume_mode = APP_UI_MODE_CLOCK;
static bool s_alarm_tone_resume_player = false;
static bool s_alarm_tone_resume_bt = false;
static bool s_alarm_tone_preview_pending = false;
static int64_t s_alarm_tone_preview_change_us = 0;
static uint8_t s_alarm_file_count = 0;
static bool s_alarm_refresh_pending = false;
static bool s_alarm_time_editing = false;
static uint8_t s_alarm_time_hour = 0;
static uint8_t s_alarm_time_min = 0;
static uint8_t s_alarm_time_select = 0; // 0=hours, 1=minutes
static int64_t s_forbidden_until_us = 0;

static int64_t menu_now_us(void)
{
    return esp_timer_get_time();
}

static void menu_request_cfg_update(void)
{
    if (!s_cfg) {
        return;
    }
    app_config_t cfg_copy = *s_cfg;
    config_owner_request_update(&cfg_copy);
}

static void menu_alarm_request_refresh(void)
{
    s_alarm_refresh_pending = true;
}

static void menu_alarm_refresh_files(void)
{
    if (app_get_ui_mode() == APP_UI_MODE_BLUETOOTH) {
        s_alarm_file_count = 0;
        return;
    }
    if (audio_player_get_state() == PLAYER_STATE_PLAYING) {
        // Avoid SD scan while player is streaming from SD.
        return;
    }
    if (!storage_sd_is_mounted()) {
        storage_sd_init();
    }
    s_alarm_file_count = alarm_sound_get_file_count();
    if (s_alarm_file_count > 0 && s_cfg && s_cfg->alarm_tone > s_alarm_file_count) {
        s_cfg->alarm_tone = s_alarm_file_count;
        menu_request_cfg_update();
    }
}

static void menu_alarm_tone_preview_enter(void)
{
    menu_alarm_request_refresh();

    s_alarm_tone_resume_mode = app_get_ui_mode();
    s_alarm_tone_resume_player = false;
    s_alarm_tone_resume_bt = false;

    {
        audio_player_state_t state = audio_player_get_state();
        if (state == PLAYER_STATE_PLAYING) {
            audio_player_stop();
            s_alarm_tone_resume_player = true;
        } else if (state == PLAYER_STATE_PAUSED) {
            audio_player_stop();
            s_alarm_tone_resume_player = false;
        }
    }

    if (bt_sink_is_playing()) {
        if (bt_avrc_is_connected()) {
            bt_avrc_send_command(BT_AVRC_CMD_PAUSE);
        }
        s_alarm_tone_resume_bt = true;
    }

    alarm_sound_stop();
    audio_stop();
    audio_i2s_write_silence(50);
    audio_i2s_reset();
    s_alarm_tone_preview_pending = true;
    s_alarm_tone_preview_change_us = menu_now_us();
}

static void menu_alarm_tone_preview_exit(void)
{
    alarm_sound_stop();
    audio_stop();
    audio_i2s_write_silence(50);
    audio_i2s_reset();

    if (s_alarm_tone_resume_player) {
        audio_player_play();
    }
    if (s_alarm_tone_resume_bt) {
        if (bt_avrc_is_connected()) {
            bt_avrc_send_command(BT_AVRC_CMD_PLAY);
        }
    }

    s_alarm_tone_resume_mode = APP_UI_MODE_CLOCK;
    s_alarm_tone_resume_player = false;
    s_alarm_tone_resume_bt = false;
    s_alarm_tone_preview_pending = false;
    s_alarm_tone_preview_change_us = 0;
}

static void menu_touch(void)
{
    s_menu_last_activity_us = menu_now_us();
}

static void menu_show_forbidden(void)
{
    s_forbidden_until_us = menu_now_us() + MENU_FORBIDDEN_US;
}

static void menu_brightness_touch(void)
{
    s_brightness_dirty = true;
    s_brightness_last_change_us = menu_now_us();
}

static void menu_brightness_commit(bool force)
{
    if (!s_brightness_dirty || !s_cfg) {
        return;
    }
    int64_t now = menu_now_us();
    if (!force && (now - s_brightness_last_change_us) < BRIGHTNESS_SAVE_IDLE_US) {
        return;
    }
    menu_request_cfg_update();
    s_brightness_dirty = false;
}

static void menu_eq_touch(void)
{
    s_eq_dirty = true;
    s_eq_last_change_us = menu_now_us();
}

static void menu_eq_commit(bool force)
{
    if (!s_eq_dirty || !s_cfg) {
        return;
    }
    int64_t now = menu_now_us();
    if (!force && (now - s_eq_last_change_us) < EQ_SAVE_IDLE_US) {
        return;
    }
    menu_request_cfg_update();
    s_eq_dirty = false;
}

static void menu_set_root_item(menu_root_item_t item)
{
    if (item >= MENU_ROOT_COUNT) {
        item = MENU_ROOT_CLOCK;
    }
    s_menu_root_item = item;
}

static void menu_set_setting_item(menu_set_item_t item)
{
    if (item >= MENU_SET_COUNT) {
        item = MENU_SET_TIME;
    }
    s_menu_set_item = item;
}

static void menu_set_alarm_item(menu_alarm_item_t item)
{
    if (item >= MENU_ALARM_COUNT) {
        item = MENU_ALARM_ENABLE;
    }
    s_menu_alarm_item = item;
}

static void menu_alarm_time_enter(void)
{
    if (!s_cfg) {
        return;
    }
    s_alarm_time_editing = true;
    s_alarm_time_hour = s_cfg->alarm_hour;
    s_alarm_time_min = s_cfg->alarm_min;
    s_alarm_time_select = 0;
}

static void menu_alarm_time_exit(void)
{
    if (!s_cfg) {
        s_alarm_time_editing = false;
        return;
    }
    if (s_alarm_time_editing) {
        s_cfg->alarm_hour = s_alarm_time_hour;
        s_cfg->alarm_min = s_alarm_time_min;
        menu_request_cfg_update();
        alarm_set(s_cfg->alarm_hour, s_cfg->alarm_min, s_cfg->alarm_enabled,
                  (alarm_mode_t)s_cfg->alarm_mode);
    }
    s_alarm_time_editing = false;
}

static void menu_next_root(int delta)
{
    int idx = (int)s_menu_root_item + delta;
    while (idx < 0) {
        idx += MENU_ROOT_COUNT;
    }
    idx %= MENU_ROOT_COUNT;
    menu_set_root_item((menu_root_item_t)idx);
}

static void menu_next_setting(int delta)
{
    int idx = (int)s_menu_set_item + delta;
    while (idx < 0) {
        idx += MENU_SET_COUNT;
    }
    idx %= MENU_SET_COUNT;
    menu_set_setting_item((menu_set_item_t)idx);
}

static void menu_next_alarm(int delta)
{
    int idx = (int)s_menu_alarm_item + delta;
    while (idx < 0) {
        idx += MENU_ALARM_COUNT;
    }
    idx %= MENU_ALARM_COUNT;
    menu_set_alarm_item((menu_alarm_item_t)idx);
}

static void menu_render_item(void)
{
    char text[5] = "    ";
    switch (s_menu_set_item) {
        case MENU_SET_TIME:
            memcpy(text, "CLOC", 4);
            break;
        case MENU_SET_ALARM_MENU:
            memcpy(text, "ALr ", 4);
            break;
        case MENU_SET_BRIGHTNESS:
            memcpy(text, "brIt", 4);
            break;
        case MENU_SET_WEB:
            memcpy(text, s_cfg && s_cfg->web_enabled ? "InOn" : "InOF", 4);
            break;
        case MENU_SET_POWER_SAVE:
            memcpy(text, s_cfg && s_cfg->power_save_enabled ? "POn " : "POFF", 4);
            break;
        default:
            memcpy(text, "SEt ", 4);
            break;
    }
    display_set_text(text, false);
}

static void menu_render_alarm_item(void)
{
    char text[5] = "    ";
    switch (s_menu_alarm_item) {
        case MENU_ALARM_ENABLE:
            memcpy(text, s_cfg && s_cfg->alarm_enabled ? "ALOn" : "ALOF", 4);
            break;
        case MENU_ALARM_TIME:
            memcpy(text, "ALt ", 4);
            break;
        case MENU_ALARM_VOLUME:
            memcpy(text, "ALvL", 4);
            break;
        case MENU_ALARM_TONE:
            memcpy(text, "ton ", 4);
            break;
        case MENU_ALARM_REPEAT:
            memcpy(text, "rEP ", 4);
            break;
        case MENU_ALARM_MODE:
            memcpy(text, "AtYP", 4);
            break;
        default:
            memcpy(text, "ALr ", 4);
            break;
    }
    display_set_text(text, false);
}

static void menu_render_root(void)
{
    char text[5] = "    ";
    switch (s_menu_root_item) {
        case MENU_ROOT_CLOCK:
            memcpy(text, "CLCK", 4);
            break;
        case MENU_ROOT_PLAYER:
            memcpy(text, "PLYR", 4);
            break;
        case MENU_ROOT_BLUETOOTH:
            memcpy(text, "BLUE", 4);
            break;
        case MENU_ROOT_RADIO:
            memcpy(text, "RAD ", 4);
            break;
        case MENU_ROOT_EQ:
            memcpy(text, "EqUA", 4);
            break;
        case MENU_ROOT_SETTINGS:
        default:
            memcpy(text, "SEt ", 4);
            break;
    }
    display_set_text(text, false);
}

static void menu_render_brightness(void)
{
    if (!s_display_brightness) {
        return;
    }
    uint32_t percent = ((uint32_t)(*s_display_brightness) * 100U + 127U) / 255U;
    if (percent > 100) {
        percent = 100;
    }
    char text[5];
    snprintf(text, sizeof(text), "b%03u", (unsigned)percent);
    display_set_text(text, false);
}

static void menu_render_eq(void)
{
    if (!s_cfg) {
        return;
    }
    uint8_t value = (s_eq_select == 0) ? s_cfg->eq_low : s_cfg->eq_high;
    char text[4];
    text[0] = (s_eq_select == 0) ? 'L' : 'H';
    text[1] = (s_eq_select == 0) ? 'o' : 'i';
    text[2] = (char)('0' + (value / 10));
    text[3] = (char)('0' + (value % 10));
    display_set_text(text, false);
}

static void menu_render_alarm_time(void)
{
    if (!s_cfg) {
        return;
    }
    uint8_t hour = s_alarm_time_editing ? s_alarm_time_hour : s_cfg->alarm_hour;
    uint8_t min = s_alarm_time_editing ? s_alarm_time_min : s_cfg->alarm_min;
    char text[4] = {
        (char)('0' + (hour / 10)),
        (char)('0' + (hour % 10)),
        (char)('0' + (min / 10)),
        (char)('0' + (min % 10))
    };

    if (s_alarm_time_editing) {
        int64_t now = menu_now_us();
        bool blink_on = ((now / 500000) % 2) == 0;
        if (!blink_on) {
            if (s_alarm_time_select == 0) {
                text[0] = ' ';
                text[1] = ' ';
            } else {
                text[2] = ' ';
                text[3] = ' ';
            }
        }
    }

    display_set_text(text, true);
}

static void menu_render_alarm_mode(void)
{
    if (!s_cfg) {
        return;
    }
    char text[5] = "    ";
    switch (s_cfg->alarm_mode) {
        case ALARM_MODE_ONCE:
            memcpy(text, "ONCE", 4);
            break;
        case ALARM_MODE_WEEKDAYS:
            memcpy(text, "5DAY", 4);
            break;
        case ALARM_MODE_DAILY:
        default:
            memcpy(text, "7DAY", 4);
            break;
    }
    display_set_text(text, false);
}

static void menu_render_alarm_tone(void)
{
    if (!s_cfg) {
        return;
    }
    if (s_alarm_file_count == 0) {
        display_set_text("t-- ", false);
        return;
    }
    uint8_t tone = s_cfg->alarm_tone;
    if (tone < 1) {
        tone = 1;
    } else if (tone > s_alarm_file_count) {
        tone = s_alarm_file_count;
    }
    char text[5];
    text[0] = 't';
    text[1] = (char)('0' + (tone / 10));
    text[2] = (char)('0' + (tone % 10));
    text[3] = ' ';
    display_set_text(text, false);
}

static void menu_render_alarm_volume(void)
{
    if (!s_cfg) {
        return;
    }
    uint8_t value = s_cfg->alarm_volume;
    if (value > APP_VOLUME_MAX) {
        value = APP_VOLUME_MAX;
    }
    char text[5];
    text[0] = 'A';
    text[1] = 'v';
    text[2] = (char)('0' + (value / 10));
    text[3] = (char)('0' + (value % 10));
    display_set_text(text, false);
}

static void menu_render_alarm_repeat(void)
{
    if (!s_cfg) {
        return;
    }
    uint8_t value = s_cfg->alarm_repeat;
    if (value < 1) {
        value = 1;
    } else if (value > 5) {
        value = 5;
    }
    char text[5];
    text[0] = 'r';
    text[1] = 'E';
    text[2] = '0';
    text[3] = (char)('0' + value);
    display_set_text(text, false);
}

void ui_menu_init(app_config_t *cfg, uint8_t *display_brightness)
{
    s_cfg = cfg;
    s_display_brightness = display_brightness;
    s_menu_active = false;
    s_menu_state = MENU_STATE_ROOT;
    s_menu_root_item = MENU_ROOT_CLOCK;
    s_menu_set_item = MENU_SET_TIME;
    s_menu_alarm_item = MENU_ALARM_ENABLE;
    s_menu_last_activity_us = 0;
    s_brightness_dirty = false;
    s_brightness_last_change_us = 0;
    s_eq_dirty = false;
    s_eq_last_change_us = 0;
    s_eq_select = 0;
}

bool ui_menu_is_active(void)
{
    return s_menu_active;
}

void ui_menu_enter(void)
{
    s_menu_active = true;
    s_menu_state = MENU_STATE_ROOT;
    s_menu_root_item = MENU_ROOT_CLOCK;
    s_menu_set_item = MENU_SET_TIME;
    s_menu_alarm_item = MENU_ALARM_ENABLE;
    menu_touch();
}

void ui_menu_exit(void)
{
    menu_brightness_commit(true);
    menu_eq_commit(true);
    if (s_menu_state == MENU_STATE_ALARM_TIME) {
        menu_alarm_time_exit();
    }
    if (s_menu_state == MENU_STATE_ALARM_TONE) {
        menu_alarm_tone_preview_exit();
    }
    s_menu_active = false;
    s_menu_state = MENU_STATE_ROOT;
    s_menu_root_item = MENU_ROOT_CLOCK;
    s_menu_set_item = MENU_SET_TIME;
    s_menu_alarm_item = MENU_ALARM_ENABLE;
    s_menu_last_activity_us = 0;
}


void ui_menu_render(void)
{
    if (!s_menu_active) {
        return;
    }
    if (s_forbidden_until_us != 0) {
        int64_t now = menu_now_us();
        if (now < s_forbidden_until_us) {
            display_set_text("frbd", false);
            return;
        }
        s_forbidden_until_us = 0;
    }
    menu_brightness_commit(false);
    menu_eq_commit(false);
    if (s_alarm_refresh_pending) {
        menu_alarm_refresh_files();
        s_alarm_refresh_pending = false;
    }
    if (s_menu_last_activity_us != 0) {
        int64_t now = menu_now_us();
        if ((now - s_menu_last_activity_us) >= MENU_TIMEOUT_US) {
            menu_brightness_commit(true);
            menu_eq_commit(true);
            if (s_menu_state == MENU_STATE_ALARM_TIME) {
                menu_alarm_time_exit();
            }
            if (s_menu_state == MENU_STATE_ALARM_TONE) {
                menu_alarm_tone_preview_exit();
            }
            ui_menu_exit();
            return;
        }
    }
    if (s_menu_state == MENU_STATE_ALARM_TONE && s_alarm_tone_preview_pending && s_cfg) {
        int64_t now = menu_now_us();
        if ((now - s_alarm_tone_preview_change_us) >= ALARM_TONE_PREVIEW_DELAY_US) {
            if (s_alarm_file_count > 0) {
                alarm_sound_play_index(s_cfg->alarm_tone, s_cfg->alarm_volume, ALARM_TONE_PREVIEW_MS);
                s_alarm_tone_preview_pending = false;
            } else if (!s_alarm_refresh_pending) {
                s_alarm_tone_preview_pending = false;
            }
        }
    }
    if (s_menu_state == MENU_STATE_BRIGHTNESS) {
        menu_render_brightness();
    } else if (s_menu_state == MENU_STATE_EQ) {
        menu_render_eq();
    } else if (s_menu_state == MENU_STATE_ALARM_VOLUME) {
        menu_render_alarm_volume();
    } else if (s_menu_state == MENU_STATE_ALARM_REPEAT) {
        menu_render_alarm_repeat();
    } else if (s_menu_state == MENU_STATE_ALARM_TONE) {
        menu_render_alarm_tone();
    } else if (s_menu_state == MENU_STATE_ALARM_TIME) {
        menu_render_alarm_time();
    } else if (s_menu_state == MENU_STATE_ALARM_MODE) {
        menu_render_alarm_mode();
    } else if (s_menu_state == MENU_STATE_ALARM_LIST) {
        menu_render_alarm_item();
    } else if (s_menu_state == MENU_STATE_ROOT) {
        menu_render_root();
    } else {
        menu_render_item();
    }
}

ui_menu_action_t ui_menu_handle_encoder(encoder_event_t event, app_ui_mode_t *out_mode)
{
    if (!s_menu_active || !s_cfg || !s_display_brightness) {
        return UI_MENU_ACTION_NONE;
    }
    menu_touch();

    if (event == ENC_EVENT_BTN_LONG) {
        if (s_menu_state == MENU_STATE_EQ) {
            s_menu_state = MENU_STATE_ROOT;
            return UI_MENU_ACTION_HANDLED;
        }
        if (s_menu_state == MENU_STATE_ALARM_TIME ||
            s_menu_state == MENU_STATE_ALARM_MODE ||
            s_menu_state == MENU_STATE_ALARM_TONE ||
            s_menu_state == MENU_STATE_ALARM_VOLUME ||
            s_menu_state == MENU_STATE_ALARM_REPEAT) {
            if (s_menu_state == MENU_STATE_ALARM_TIME) {
                menu_alarm_time_exit();
            }
            if (s_menu_state == MENU_STATE_ALARM_TONE) {
                menu_alarm_tone_preview_exit();
            }
            s_menu_state = MENU_STATE_ALARM_LIST;
            return UI_MENU_ACTION_HANDLED;
        }
        if (s_menu_state == MENU_STATE_BRIGHTNESS) {
            s_menu_state = MENU_STATE_SETTINGS_LIST;
            return UI_MENU_ACTION_HANDLED;
        }
        if (s_menu_state == MENU_STATE_ALARM_LIST) {
            s_menu_state = MENU_STATE_SETTINGS_LIST;
            return UI_MENU_ACTION_HANDLED;
        }
        if (s_menu_state == MENU_STATE_SETTINGS_LIST) {
            s_menu_state = MENU_STATE_ROOT;
            return UI_MENU_ACTION_HANDLED;
        }
        return UI_MENU_ACTION_HANDLED;
    }

    if (event == ENC_EVENT_BTN_SHORT) {
        if (s_menu_state == MENU_STATE_EQ) {
            s_eq_select = (uint8_t)(s_eq_select ^ 1U);
            return UI_MENU_ACTION_HANDLED;
        }
        if (s_menu_state == MENU_STATE_BRIGHTNESS) {
            menu_brightness_commit(true);
            s_menu_state = MENU_STATE_SETTINGS_LIST;
            return UI_MENU_ACTION_HANDLED;
        }
        if (s_menu_state == MENU_STATE_ALARM_TIME ||
            s_menu_state == MENU_STATE_ALARM_MODE ||
            s_menu_state == MENU_STATE_ALARM_TONE ||
            s_menu_state == MENU_STATE_ALARM_VOLUME ||
            s_menu_state == MENU_STATE_ALARM_REPEAT) {
            if (s_menu_state == MENU_STATE_ALARM_TIME) {
                if (s_alarm_time_select == 0) {
                    s_alarm_time_select = 1;
                    return UI_MENU_ACTION_HANDLED;
                }
                menu_alarm_time_exit();
            }
            if (s_menu_state == MENU_STATE_ALARM_TONE) {
                menu_alarm_tone_preview_exit();
            }
            s_menu_state = MENU_STATE_ALARM_LIST;
            return UI_MENU_ACTION_HANDLED;
        }

        if (s_menu_state == MENU_STATE_ROOT) {
            switch (s_menu_root_item) {
                case MENU_ROOT_CLOCK:
                    if (out_mode) {
                        *out_mode = APP_UI_MODE_CLOCK;
                    }
                    ui_menu_exit();
                    return UI_MENU_ACTION_SET_MODE;
                case MENU_ROOT_PLAYER:
                    if (out_mode) {
                        *out_mode = APP_UI_MODE_PLAYER;
                    }
                    ui_menu_exit();
                    return UI_MENU_ACTION_SET_MODE;
                case MENU_ROOT_BLUETOOTH:
                    if (out_mode) {
                        *out_mode = APP_UI_MODE_BLUETOOTH;
                    }
                    ui_menu_exit();
                    return UI_MENU_ACTION_SET_MODE;
                case MENU_ROOT_RADIO:
                    if (out_mode) {
                        *out_mode = APP_UI_MODE_RADIO;
                    }
                    ui_menu_exit();
                    return UI_MENU_ACTION_SET_MODE;
                case MENU_ROOT_EQ:
                    s_menu_state = MENU_STATE_EQ;
                    s_eq_select = 0;
                    return UI_MENU_ACTION_HANDLED;
                case MENU_ROOT_SETTINGS:
                default:
                    s_menu_state = MENU_STATE_SETTINGS_LIST;
                    return UI_MENU_ACTION_HANDLED;
            }
        }

        if (s_menu_state == MENU_STATE_SETTINGS_LIST) {
            switch (s_menu_set_item) {
                case MENU_SET_TIME:
                    ui_menu_exit();
                    return UI_MENU_ACTION_ENTER_TIME_SETTING;
                case MENU_SET_ALARM_MENU:
                    menu_alarm_request_refresh();
                    s_menu_state = MENU_STATE_ALARM_LIST;
                    break;
                case MENU_SET_BRIGHTNESS:
                    s_menu_state = MENU_STATE_BRIGHTNESS;
                    break;
                case MENU_SET_WEB:
                    s_cfg->web_enabled = !s_cfg->web_enabled;
                    menu_request_cfg_update();
                    wifi_set_web_enabled(s_cfg->web_enabled);
                    break;
                case MENU_SET_POWER_SAVE:
                    s_cfg->power_save_enabled = !s_cfg->power_save_enabled;
                    menu_request_cfg_update();
                    power_manager_set_autonomous(s_cfg->power_save_enabled);
                    break;
                default:
                    break;
            }
            return UI_MENU_ACTION_HANDLED;
        }
        if (s_menu_state == MENU_STATE_ALARM_LIST) {
            switch (s_menu_alarm_item) {
                case MENU_ALARM_ENABLE:
                    s_cfg->alarm_enabled = !s_cfg->alarm_enabled;
                    menu_request_cfg_update();
                    alarm_set(s_cfg->alarm_hour, s_cfg->alarm_min, s_cfg->alarm_enabled,
                              (alarm_mode_t)s_cfg->alarm_mode);
                    break;
                case MENU_ALARM_TIME:
                    s_menu_state = MENU_STATE_ALARM_TIME;
                    menu_alarm_time_enter();
                    break;
                case MENU_ALARM_VOLUME:
                    s_menu_state = MENU_STATE_ALARM_VOLUME;
                    break;
                case MENU_ALARM_TONE:
                    if (app_get_ui_mode() == APP_UI_MODE_BLUETOOTH) {
                        menu_show_forbidden();
                    } else {
                        s_menu_state = MENU_STATE_ALARM_TONE;
                        menu_alarm_tone_preview_enter();
                    }
                    break;
                case MENU_ALARM_REPEAT:
                    s_menu_state = MENU_STATE_ALARM_REPEAT;
                    break;
                case MENU_ALARM_MODE:
                    s_menu_state = MENU_STATE_ALARM_MODE;
                    break;
                default:
                    break;
            }
            return UI_MENU_ACTION_HANDLED;
        }
        return UI_MENU_ACTION_HANDLED;
    }

    if (event == ENC_EVENT_CW || event == ENC_EVENT_CCW) {
        int delta = (event == ENC_EVENT_CW) ? 1 : -1;
        if (s_menu_state == MENU_STATE_ROOT) {
            menu_next_root(delta);
        } else if (s_menu_state == MENU_STATE_SETTINGS_LIST) {
            menu_next_setting(delta);
        } else if (s_menu_state == MENU_STATE_ALARM_LIST) {
            menu_next_alarm(delta);
        } else if (s_menu_state == MENU_STATE_BRIGHTNESS) {
            int value = *s_display_brightness + (delta * BRIGHTNESS_STEP);
            if (value < 0) {
                value = 0;
            } else if (value > 255) {
                value = 255;
            }
            *s_display_brightness = (uint8_t)value;
            display_set_brightness(*s_display_brightness);
            s_cfg->display_brightness = *s_display_brightness;
            menu_brightness_touch();
        } else if (s_menu_state == MENU_STATE_EQ) {
            uint8_t *target = (s_eq_select == 0) ? &s_cfg->eq_low : &s_cfg->eq_high;
            int value = (int)(*target) + delta;
            if (value < 0) {
                value = 0;
            } else if (value > 30) {
                value = 30;
            }
            *target = (uint8_t)value;
            audio_eq_set_steps(s_cfg->eq_low, s_cfg->eq_high);
            menu_eq_touch();
        } else if (s_menu_state == MENU_STATE_ALARM_TIME) {
            if (s_alarm_time_select == 0) {
                int hour = (int)(s_alarm_time_editing ? s_alarm_time_hour : s_cfg->alarm_hour);
                hour += delta;
                if (hour < 0) {
                    hour = 23;
                } else if (hour > 23) {
                    hour = 0;
                }
                s_alarm_time_hour = (uint8_t)hour;
            } else {
                int min = (int)(s_alarm_time_editing ? s_alarm_time_min : s_cfg->alarm_min);
                min += delta;
                if (min < 0) {
                    min = 59;
                } else if (min > 59) {
                    min = 0;
                }
                s_alarm_time_min = (uint8_t)min;
            }
        } else if (s_menu_state == MENU_STATE_ALARM_MODE) {
            int mode = (int)s_cfg->alarm_mode + delta;
            if (mode < (int)ALARM_MODE_ONCE) {
                mode = (int)ALARM_MODE_DAILY;
            } else if (mode > (int)ALARM_MODE_DAILY) {
                mode = (int)ALARM_MODE_ONCE;
            }
            s_cfg->alarm_mode = (uint8_t)mode;
            menu_request_cfg_update();
            alarm_set(s_cfg->alarm_hour, s_cfg->alarm_min, s_cfg->alarm_enabled,
                      (alarm_mode_t)s_cfg->alarm_mode);
        } else if (s_menu_state == MENU_STATE_ALARM_VOLUME) {
            int value = (int)s_cfg->alarm_volume + delta;
            if (value < 1) {
                value = 1;
            } else if (value > APP_VOLUME_MAX) {
                value = APP_VOLUME_MAX;
            }
            s_cfg->alarm_volume = (uint8_t)value;
            menu_request_cfg_update();
        } else if (s_menu_state == MENU_STATE_ALARM_REPEAT) {
            int value = (int)s_cfg->alarm_repeat + delta;
            if (value < 1) {
                value = 1;
            } else if (value > 5) {
                value = 5;
            }
            s_cfg->alarm_repeat = (uint8_t)value;
            menu_request_cfg_update();
        } else if (s_menu_state == MENU_STATE_ALARM_TONE) {
            if (s_alarm_file_count > 0) {
                int tone = (int)s_cfg->alarm_tone + delta;
                if (tone < 1) {
                    tone = s_alarm_file_count;
                } else if (tone > s_alarm_file_count) {
                    tone = 1;
                }
                s_cfg->alarm_tone = (uint8_t)tone;
                menu_request_cfg_update();
                alarm_sound_stop();
                audio_stop();
                audio_i2s_write_silence(50);
                audio_i2s_reset();
                s_alarm_tone_preview_pending = true;
                s_alarm_tone_preview_change_us = menu_now_us();
            }
        }
        return UI_MENU_ACTION_HANDLED;
    }

    return UI_MENU_ACTION_NONE;
}
