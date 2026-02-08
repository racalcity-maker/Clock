#include "alarm_actions.h"

#include "audio_player.h"
#include "bluetooth_sink.h"
#include "bt_app_core.h"
#include "bt_avrc.h"
#include "radio_rda5807.h"
#include "storage_sd_spi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static app_ui_mode_t s_resume_mode = APP_UI_MODE_CLOCK;
static bool s_resume_player = false;
static bool s_resume_bt = false;
static bool s_resume_radio = false;
static bool s_resume_pending = false;
static bool s_alarm_sd_mounted = false;
static portMUX_TYPE s_resume_lock = portMUX_INITIALIZER_UNLOCKED;

static void alarm_actions_resume_now(app_ui_mode_t resume_mode, bool resume_player, bool resume_bt)
{
    if (resume_mode == APP_UI_MODE_PLAYER && resume_player) {
        if (!audio_player_is_ready()) {
            audio_player_init("/sdcard/music");
            audio_player_rescan();
        }
        audio_player_play();
    } else if (resume_mode == APP_UI_MODE_BLUETOOTH && resume_bt) {
        if (bt_avrc_is_connected()) {
            bt_avrc_send_command(BT_AVRC_CMD_PLAY);
        }
    } else if (resume_mode == APP_UI_MODE_RADIO) {
        radio_rda5807_set_enabled(true);
        radio_rda5807_set_muted(false);
    }
}

void alarm_actions_on_trigger(void)
{
    app_ui_mode_t mode = app_get_ui_mode();
    portENTER_CRITICAL(&s_resume_lock);
    s_resume_mode = mode;
    s_resume_player = false;
    s_resume_bt = false;
    s_resume_radio = false;
    s_resume_pending = false;
    s_alarm_sd_mounted = false;
    portEXIT_CRITICAL(&s_resume_lock);

    if (mode == APP_UI_MODE_PLAYER) {
        bool resume_player = false;
        audio_player_state_t state = audio_player_get_state();
        if (state == PLAYER_STATE_PLAYING) {
            resume_player = true;
        }
        portENTER_CRITICAL(&s_resume_lock);
        s_resume_player = resume_player;
        portEXIT_CRITICAL(&s_resume_lock);
        audio_player_stop();
        audio_player_shutdown();
        return;
    }

    if (mode == APP_UI_MODE_BLUETOOTH) {
        if (bt_avrc_is_connected()) {
            bt_avrc_send_command(BT_AVRC_CMD_PAUSE);
        }
        bt_i2s_task_shut_down();
        for (int i = 0; i < 50 && bt_i2s_task_is_running(); ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        bool resume_bt = bt_sink_is_playing() || bt_avrc_is_connected();
        portENTER_CRITICAL(&s_resume_lock);
        s_resume_bt = resume_bt;
        portEXIT_CRITICAL(&s_resume_lock);
        return;
    }

    if (mode == APP_UI_MODE_RADIO) {
        portENTER_CRITICAL(&s_resume_lock);
        s_resume_radio = true;
        portEXIT_CRITICAL(&s_resume_lock);
        radio_rda5807_set_muted(true);
        radio_rda5807_set_enabled(false);
        if (!storage_sd_is_mounted()) {
            if (storage_sd_init() == ESP_OK) {
                s_alarm_sd_mounted = true;
            }
        }
        return;
    }
}

void alarm_actions_on_ack(void)
{
    app_ui_mode_t resume_mode;
    bool resume_player;
    bool resume_bt;
    bool resume_radio;

    portENTER_CRITICAL(&s_resume_lock);
    resume_mode = s_resume_mode;
    resume_player = s_resume_player;
    resume_bt = s_resume_bt;
    resume_radio = s_resume_radio;
    portEXIT_CRITICAL(&s_resume_lock);

    if (resume_mode == APP_UI_MODE_CLOCK) {
        portENTER_CRITICAL(&s_resume_lock);
        s_resume_player = false;
        s_resume_bt = false;
        s_resume_radio = false;
        s_alarm_sd_mounted = false;
        s_resume_pending = false;
        portEXIT_CRITICAL(&s_resume_lock);
        return;
    }

    if (app_get_ui_mode() != resume_mode) {
        portENTER_CRITICAL(&s_resume_lock);
        s_resume_pending = true;
        portEXIT_CRITICAL(&s_resume_lock);
        app_request_ui_mode(resume_mode);
        return;
    }

    bool unmount_sd = false;
    portENTER_CRITICAL(&s_resume_lock);
    s_resume_mode = APP_UI_MODE_CLOCK;
    s_resume_player = false;
    s_resume_bt = false;
    s_resume_radio = false;
    unmount_sd = s_alarm_sd_mounted;
    s_alarm_sd_mounted = false;
    s_resume_pending = false;
    portEXIT_CRITICAL(&s_resume_lock);

    (void)resume_radio;
    alarm_actions_resume_now(resume_mode, resume_player, resume_bt);
    if (unmount_sd) {
        storage_sd_unmount();
    }
}

void alarm_actions_poll(void)
{
    app_ui_mode_t resume_mode;
    bool resume_player;
    bool resume_bt;
    bool resume_radio;
    bool resume_pending;

    portENTER_CRITICAL(&s_resume_lock);
    resume_pending = s_resume_pending;
    resume_mode = s_resume_mode;
    resume_player = s_resume_player;
    resume_bt = s_resume_bt;
    resume_radio = s_resume_radio;
    portEXIT_CRITICAL(&s_resume_lock);

    if (!resume_pending) {
        return;
    }
    if (app_get_ui_mode() != resume_mode) {
        return;
    }

    portENTER_CRITICAL(&s_resume_lock);
    if (!s_resume_pending || s_resume_mode != resume_mode) {
        portEXIT_CRITICAL(&s_resume_lock);
        return;
    }
    bool unmount_sd = s_alarm_sd_mounted;
    s_alarm_sd_mounted = false;
    s_resume_mode = APP_UI_MODE_CLOCK;
    s_resume_player = false;
    s_resume_bt = false;
    s_resume_radio = false;
    s_resume_pending = false;
    portEXIT_CRITICAL(&s_resume_lock);

    (void)resume_radio;
    alarm_actions_resume_now(resume_mode, resume_player, resume_bt);
    if (unmount_sd) {
        storage_sd_unmount();
    }
}
