/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_log.h"

#include "bt_app_core.h"
#include "bt_app_av.h"
#include "bluetooth_sink.h"
#include "audio_spectrum.h"
#include "audio_pcm5102.h"
#include "esp_a2dp_api.h"

/* Application layer causes delay value */
#define APP_DELAY_VALUE  50  /* 5 ms */

/*******************************
 * STATIC FUNCTION DECLARATIONS
 ******************************/

/* a2dp event handler */
static void bt_av_hdl_a2d_evt(uint16_t event, void *p_param);

/********************************
 * STATIC FUNCTION DEFINITIONS
 *******************************/

static void bt_av_hdl_a2d_evt(uint16_t event, void *p_param)
{
    esp_a2d_cb_param_t *a2d = NULL;

    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        a2d = (esp_a2d_cb_param_t *)(p_param);
        if (a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            bt_i2s_task_shut_down();
            bt_app_core_reset_ringbuffer();
        }
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT: {
        a2d = (esp_a2d_cb_param_t *)(p_param);
        if (ESP_A2D_AUDIO_STATE_STARTED == a2d->audio_stat.state) {
            if (!bt_i2s_task_is_running()) {
                bt_app_core_reset_ringbuffer();
                bt_i2s_task_start_up();
            }
        } else if (ESP_A2D_AUDIO_STATE_SUSPEND == a2d->audio_stat.state) {
            bt_i2s_task_shut_down();
            bt_app_core_reset_ringbuffer();
        }
        break;
    }
    case ESP_A2D_AUDIO_CFG_EVT: {
        a2d = (esp_a2d_cb_param_t *)(p_param);
        if (a2d->audio_cfg.mcc.type == ESP_A2D_MCT_SBC) {
            int sample_rate = 16000;
            char oct0 = a2d->audio_cfg.mcc.cie.sbc[0];
            if (oct0 & (0x01 << 6)) {
                sample_rate = 32000;
            } else if (oct0 & (0x01 << 5)) {
                sample_rate = 44100;
            } else if (oct0 & (0x01 << 4)) {
                sample_rate = 48000;
            }
            audio_i2s_set_sample_rate((uint32_t)sample_rate);
            audio_spectrum_set_sample_rate((uint32_t)sample_rate);
        }
        break;
    }
    case ESP_A2D_PROF_STATE_EVT:
    case ESP_A2D_SNK_PSC_CFG_EVT:
    case ESP_A2D_SNK_SET_DELAY_VALUE_EVT:
        break;
    case ESP_A2D_SNK_GET_DELAY_VALUE_EVT: {
        a2d = (esp_a2d_cb_param_t *)(p_param);
        esp_a2d_sink_set_delay_value(a2d->a2d_get_delay_value_stat.delay_value + APP_DELAY_VALUE);
        break;
    }
    default:
        ESP_LOGE(BT_AV_TAG, "%s unhandled event: %d", __func__, event);
        break;
    }
}

/********************************
 * EXTERNAL FUNCTION DEFINITIONS
 *******************************/

void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    bool dispatched = false;
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
    case ESP_A2D_AUDIO_STATE_EVT:
    case ESP_A2D_AUDIO_CFG_EVT:
    case ESP_A2D_PROF_STATE_EVT:
    case ESP_A2D_SNK_PSC_CFG_EVT:
    case ESP_A2D_SNK_SET_DELAY_VALUE_EVT:
    case ESP_A2D_SNK_GET_DELAY_VALUE_EVT:
        dispatched = bt_app_work_dispatch(bt_av_hdl_a2d_evt, event, param, sizeof(esp_a2d_cb_param_t), NULL);
        break;
    default:
        ESP_LOGE(BT_AV_TAG, "Invalid A2DP event: %d", event);
        break;
    }
    if (!dispatched) {
        ESP_LOGW(BT_AV_TAG, "A2DP dispatch failed: evt=%d", event);
    }
}

void bt_app_a2d_data_cb(const uint8_t *data, uint32_t len)
{
    if (!bt_i2s_task_is_running()) {
        bt_app_core_reset_ringbuffer();
        bt_i2s_task_start_up();
    }
    bt_sink_note_audio_data();
    write_ringbuf(data, len);
}
