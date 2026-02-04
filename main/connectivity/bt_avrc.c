/*
 * Full AVRCP implementation adapted from the ESP-IDF A2DP sink example.
 */

#include "bt_avrc.h"

#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "bt_avrc";

static bool s_avrc_ready = false;
static bool s_avrc_connected = false;
static uint8_t s_cmd_label = 0;
static bool s_tg_ready = false;
static bool s_tg_connected = false;
static bool s_volume_notify = false;
static uint8_t s_volume = 0x7f; /* 0-0x7f */
static bt_avrc_volume_cb_t s_volume_cb = NULL;

#if defined(CONFIG_BT_ENABLED) && CONFIG_BT_ENABLED && \
    defined(CONFIG_BT_CLASSIC_ENABLED) && CONFIG_BT_CLASSIC_ENABLED && \
    defined(CONFIG_BT_A2DP_ENABLE) && CONFIG_BT_A2DP_ENABLE

#include "bt_app_core.h"
#include "esp_avrc_api.h"
#include "sys/lock.h"

#define BT_RC_TG_TAG    "RC_TG"
#define BT_RC_CT_TAG    "RC_CT"

static _lock_t s_volume_lock;

static uint8_t vol_255_to_127(uint8_t volume)
{
    uint16_t v = (uint16_t)volume;
    v = (uint16_t)((v * 0x7fU + 127U) / 255U);
    if (v > 0x7fU) {
        v = 0x7fU;
    }
    return (uint8_t)v;
}

static uint8_t vol_127_to_255(uint8_t volume)
{
    uint16_t v = (uint16_t)volume;
    if (v > 0x7fU) {
        v = 0x7fU;
    }
    v = (uint16_t)((v * 255U + 0x3fU) / 0x7fU);
    if (v > 255U) {
        v = 255U;
    }
    return (uint8_t)v;
}

static void bt_av_hdl_avrc_ct_evt(uint16_t event, void *p_param)
{
    esp_avrc_ct_cb_param_t *rc = (esp_avrc_ct_cb_param_t *)(p_param);

    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT: {
        s_avrc_connected = rc->conn_stat.connected;
        break;
    }
    case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
    case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
        break;
    default:
        ESP_LOGE(BT_RC_CT_TAG, "%s unhandled event: %d", __func__, event);
        break;
    }
}

static void bt_av_hdl_avrc_tg_evt(uint16_t event, void *p_param)
{
    esp_avrc_tg_cb_param_t *rc = (esp_avrc_tg_cb_param_t *)(p_param);

    switch (event) {
    case ESP_AVRC_TG_CONNECTION_STATE_EVT: {
        s_tg_connected = rc->conn_stat.connected;
        if (!s_tg_connected) {
            s_volume_notify = false;
        }
        break;
    }
    case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT:
        break;
    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT: {
        uint8_t vol127 = rc->set_abs_vol.volume;
        if (vol127 > 0x7f) {
            vol127 = 0x7f;
        }
        _lock_acquire(&s_volume_lock);
        s_volume = vol127;
        _lock_release(&s_volume_lock);
        if (s_volume_cb) {
            s_volume_cb(vol_127_to_255(vol127));
        }
        break;
    }
    case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT: {
        if (rc->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
            s_volume_notify = true;
            esp_avrc_rn_param_t rn_param;
            _lock_acquire(&s_volume_lock);
            rn_param.volume = s_volume;
            _lock_release(&s_volume_lock);
            esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_INTERIM, &rn_param);
        }
        break;
    }
    case ESP_AVRC_TG_REMOTE_FEATURES_EVT:
        break;
    default:
        ESP_LOGE(BT_RC_TG_TAG, "%s unhandled event: %d", __func__, event);
        break;
    }
}

static void bt_avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_CT_METADATA_RSP_EVT:
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
        (void)param;
        break;
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
    case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
    case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
        bt_app_work_dispatch(bt_av_hdl_avrc_ct_evt, event, param, sizeof(esp_avrc_ct_cb_param_t), NULL);
        break;
    default:
        ESP_LOGE(BT_RC_CT_TAG, "Invalid AVRC event: %d", event);
        break;
    }
}

static void bt_avrc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_TG_CONNECTION_STATE_EVT:
    case ESP_AVRC_TG_REMOTE_FEATURES_EVT:
    case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT:
    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:
    case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
    case ESP_AVRC_TG_SET_PLAYER_APP_VALUE_EVT:
        bt_app_work_dispatch(bt_av_hdl_avrc_tg_evt, event, param, sizeof(esp_avrc_tg_cb_param_t), NULL);
        break;
    default:
        ESP_LOGE(BT_RC_TG_TAG, "Invalid AVRC event: %d", event);
        break;
    }
}

esp_err_t bt_avrc_init(void)
{
    esp_err_t err = esp_avrc_ct_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "avrcp init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_avrc_ct_register_callback(bt_avrc_ct_cb);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "avrcp ct cb reg failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_avrc_tg_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "avrcp tg init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_avrc_tg_register_callback(bt_avrc_tg_cb);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "avrcp tg cb reg failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_avrc_rn_evt_cap_mask_t evt_set = {0};
    esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt_set, ESP_AVRC_RN_VOLUME_CHANGE);
    err = esp_avrc_tg_set_rn_evt_cap(&evt_set);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "avrcp tg evt cap failed: %s", esp_err_to_name(err));
    }

    s_avrc_ready = true;
    s_tg_ready = true;
    return ESP_OK;
}

void bt_avrc_deinit(void)
{
#if defined(CONFIG_BT_ENABLED) && CONFIG_BT_ENABLED && \
    defined(CONFIG_BT_CLASSIC_ENABLED) && CONFIG_BT_CLASSIC_ENABLED && \
    defined(CONFIG_BT_A2DP_ENABLE) && CONFIG_BT_A2DP_ENABLE
    if (s_avrc_ready) {
        esp_avrc_ct_deinit();
    }
    if (s_tg_ready) {
        esp_avrc_tg_deinit();
    }
#endif
    s_avrc_ready = false;
    s_avrc_connected = false;
    s_tg_ready = false;
    s_tg_connected = false;
    s_volume_notify = false;
    s_cmd_label = 0;
}

static esp_err_t bt_avrc_send_passthrough(uint8_t key_code)
{
    if (!s_avrc_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t label = (uint8_t)(s_cmd_label++ & 0x0F);
    esp_err_t err = esp_avrc_ct_send_passthrough_cmd(label,
                                                     key_code,
                                                     ESP_AVRC_PT_CMD_STATE_PRESSED);
    if (err != ESP_OK) {
        return err;
    }
    return esp_avrc_ct_send_passthrough_cmd(label,
                                            key_code,
                                            ESP_AVRC_PT_CMD_STATE_RELEASED);
}

esp_err_t bt_avrc_send_command(bt_avrc_cmd_t cmd)
{
    switch (cmd) {
    case BT_AVRC_CMD_PLAY:
        return bt_avrc_send_passthrough(ESP_AVRC_PT_CMD_PLAY);
    case BT_AVRC_CMD_PAUSE:
        return bt_avrc_send_passthrough(ESP_AVRC_PT_CMD_PAUSE);
    case BT_AVRC_CMD_NEXT:
        return bt_avrc_send_passthrough(ESP_AVRC_PT_CMD_FORWARD);
    case BT_AVRC_CMD_PREV:
        return bt_avrc_send_passthrough(ESP_AVRC_PT_CMD_BACKWARD);
    case BT_AVRC_CMD_STOP:
        return bt_avrc_send_passthrough(ESP_AVRC_PT_CMD_STOP);
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

bool bt_avrc_is_connected(void)
{
    return s_avrc_connected;
}

void bt_avrc_register_volume_cb(bt_avrc_volume_cb_t cb)
{
    s_volume_cb = cb;
}

void bt_avrc_notify_volume(uint8_t volume)
{
    if (!s_tg_ready) {
        return;
    }
    uint8_t vol127 = vol_255_to_127(volume);
    _lock_acquire(&s_volume_lock);
    s_volume = vol127;
    _lock_release(&s_volume_lock);

    if (s_tg_connected && s_volume_notify) {
        esp_avrc_rn_param_t rn_param = {
            .volume = s_volume
        };
        esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_CHANGED, &rn_param);
        s_volume_notify = false;
    }
}

#else

esp_err_t bt_avrc_init(void)
{
    ESP_LOGW(TAG, "avrcp controller disabled in sdkconfig");
    return ESP_ERR_NOT_SUPPORTED;
}

void bt_avrc_deinit(void)
{
}

esp_err_t bt_avrc_send_command(bt_avrc_cmd_t cmd)
{
    (void)cmd;
    return ESP_ERR_NOT_SUPPORTED;
}

bool bt_avrc_is_connected(void)
{
    return false;
}

void bt_avrc_register_volume_cb(bt_avrc_volume_cb_t cb)
{
    (void)cb;
}

void bt_avrc_notify_volume(uint8_t volume)
{
    (void)volume;
}

#endif
