#include "bluetooth_sink.h"

#include "bt_app_av.h"
#include "bt_app_core.h"
#include "bt_avrc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "audio_pcm5102.h"
#include "audio_tones.h"
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

static volatile bool s_bt_streaming = false;
static int64_t s_last_audio_data_us = 0;
static const int64_t BT_STREAM_TIMEOUT_US = 2000000;

static const char *TAG = "bluetooth_sink";

#if defined(CONFIG_BT_ENABLED) && CONFIG_BT_ENABLED

#include "esp_bt.h"
#include "esp_gap_bt_api.h"
#include "esp_bt_main.h"
#include "stack/btm_api.h"
#include "stack/hcidefs.h"

static bool s_bt_connected = false;
static bool s_bt_ready = false;
static bool s_discoverable_requested = false;
static esp_bd_addr_t s_connected_bda = {0};
static uint8_t s_pm_id = BTM_PM_SET_ONLY_ID;
static esp_a2d_audio_state_t s_a2d_audio_state = ESP_A2D_AUDIO_STATE_SUSPEND;
static bool s_gap_cb_registered = false;
static bool s_a2dp_cb_registered = false;
static esp_timer_handle_t s_autoconnect_timer = NULL;
static bool s_autoconnect_allowed = false;

static void bt_drop_bond(const esp_bd_addr_t bda);
static void bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
static inline void bt_last_audio_set(int64_t value);
static inline int64_t bt_last_audio_get(void);
static void bt_pm_force_active(const esp_bd_addr_t bda);
static void bt_link_policy_disable_sniff(const esp_bd_addr_t bda);
static void bt_set_link_supervision_timeout(const esp_bd_addr_t bda);
static bool bt_get_last_bonded(esp_bd_addr_t out);
static void bt_autoconnect_timer_cb(void *arg);
static void bt_autoconnect_disable(void);

#if defined(CONFIG_BT_A2DP_ENABLE) && CONFIG_BT_A2DP_ENABLE
#include "esp_a2dp_api.h"
#endif

static void bt_sink_reset_state(void)
{
    s_bt_connected = false;
    s_bt_ready = false;
    s_discoverable_requested = false;
    s_bt_streaming = false;
    s_a2d_audio_state = ESP_A2D_AUDIO_STATE_SUSPEND;
    bt_last_audio_set(0);
    memset(s_connected_bda, 0, sizeof(s_connected_bda));
    bt_autoconnect_disable();
}

#if defined(CONFIG_BT_A2DP_ENABLE) && CONFIG_BT_A2DP_ENABLE
static void bt_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    if (!param) {
        return;
    }
    switch (event) {
        case ESP_A2D_CONNECTION_STATE_EVT:
            ESP_LOGD(TAG, "a2dp conn state=%d", param->conn_stat.state);
            bool was_connected = s_bt_connected;
            if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                s_bt_connected = true;
                memcpy(s_connected_bda, param->conn_stat.remote_bda, sizeof(esp_bd_addr_t));
                if (!was_connected) {
                    audio_play_system_tone(AUDIO_SYS_TONE_BT_CONNECT);
                }
                bt_link_policy_disable_sniff(param->conn_stat.remote_bda);
                bt_pm_force_active(param->conn_stat.remote_bda);
                bt_set_link_supervision_timeout(param->conn_stat.remote_bda);
                if (s_discoverable_requested) {
                    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
                }
            } else {
                s_bt_connected = false;
                memset(s_connected_bda, 0, sizeof(s_connected_bda));
                s_bt_streaming = false;
                s_a2d_audio_state = ESP_A2D_AUDIO_STATE_SUSPEND;
                bt_last_audio_set(0);
                bt_autoconnect_disable();
                if (was_connected) {
                    audio_play_system_tone(AUDIO_SYS_TONE_BT_DISCONNECT);
                }
                if (s_discoverable_requested) {
                    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
                }
            }
            break;
        case ESP_A2D_AUDIO_STATE_EVT:
            ESP_LOGD(TAG, "a2dp audio state=%d", param->audio_stat.state);
            s_bt_streaming = (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED);
            s_a2d_audio_state = param->audio_stat.state;
            break;
        default:
            break;
    }
    bt_app_a2d_cb(event, param);
}
#endif

static void bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    if (!param) {
        return;
    }
    switch (event) {
        case ESP_BT_GAP_AUTH_CMPL_EVT:
            if (param->auth_cmpl.stat != ESP_BT_STATUS_SUCCESS) {
                bt_drop_bond(param->auth_cmpl.bda);
            }
            break;
        case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
            ESP_LOGD(TAG, "acl conn stat=%d", param->acl_conn_cmpl_stat.stat);
            if (param->acl_conn_cmpl_stat.stat == ESP_BT_STATUS_SUCCESS) {
                bt_link_policy_disable_sniff(param->acl_conn_cmpl_stat.bda);
                bt_pm_force_active(param->acl_conn_cmpl_stat.bda);
                bt_set_link_supervision_timeout(param->acl_conn_cmpl_stat.bda);
            }
            break;
        case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
            ESP_LOGD(TAG, "acl disconnect reason=0x%02x", param->acl_disconn_cmpl_stat.reason);
            if (s_bt_connected) {
                s_bt_connected = false;
                s_bt_streaming = false;
                s_a2d_audio_state = ESP_A2D_AUDIO_STATE_SUSPEND;
                bt_last_audio_set(0);
                memset(s_connected_bda, 0, sizeof(s_connected_bda));
                audio_play_system_tone(AUDIO_SYS_TONE_BT_DISCONNECT);
            }
            bt_autoconnect_disable();
            if (s_discoverable_requested) {
                esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
            }
            break;
        case ESP_BT_GAP_PIN_REQ_EVT: {
            esp_bt_gap_pin_reply(param->pin_req.bda, false, 0, NULL);
            break;
        }
        case ESP_BT_GAP_CFM_REQ_EVT:
            esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
            break;
        case ESP_BT_GAP_KEY_REQ_EVT:
            esp_bt_gap_ssp_passkey_reply(param->key_req.bda, false, 0);
            break;
        case ESP_BT_GAP_MODE_CHG_EVT:
            if (param->mode_chg.mode != 0 && s_bt_streaming) {
                bt_pm_force_active(param->mode_chg.bda);
            }
            break;
        default:
            break;
    }
}

static void bt_drop_bond(const esp_bd_addr_t bda)
{
    if (!bda) {
        return;
    }
    esp_bd_addr_t addr;
    memcpy(addr, bda, sizeof(addr));
    esp_err_t err = esp_bt_gap_remove_bond_device(addr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "remove bond failed: %s", esp_err_to_name(err));
    }
}

static inline void bt_last_audio_set(int64_t value)
{
    __atomic_store_n(&s_last_audio_data_us, value, __ATOMIC_RELAXED);
}

static inline int64_t bt_last_audio_get(void)
{
    return __atomic_load_n(&s_last_audio_data_us, __ATOMIC_RELAXED);
}

static bool bt_get_last_bonded(esp_bd_addr_t out)
{
    int dev_num = esp_bt_gap_get_bond_device_num();
    if (dev_num <= 0) {
        return false;
    }

    esp_bd_addr_t *dev_list = calloc((size_t)dev_num, sizeof(esp_bd_addr_t));
    if (!dev_list) {
        return false;
    }

    int list_num = dev_num;
    esp_err_t err = esp_bt_gap_get_bond_device_list(&list_num, dev_list);
    if (err != ESP_OK || list_num <= 0) {
        free(dev_list);
        return false;
    }

    memcpy(out, dev_list[list_num - 1], sizeof(esp_bd_addr_t));
    free(dev_list);
    return true;
}

static void bt_autoconnect_timer_cb(void *arg)
{
    (void)arg;
    bt_sink_try_connect_last();
}

static void bt_autoconnect_disable(void)
{
    s_autoconnect_allowed = false;
    if (s_autoconnect_timer) {
        ESP_LOGD(TAG, "bt autoconnect disabled");
        esp_timer_stop(s_autoconnect_timer);
    }
}

static void bt_pm_force_active(const esp_bd_addr_t bda)
{
    if (!bda) {
        return;
    }

    if (s_pm_id == BTM_PM_SET_ONLY_ID) {
        if (BTM_PmRegister(BTM_PM_REG_SET, &s_pm_id, NULL) != BTM_SUCCESS) {
            ESP_LOGW(TAG, "bt pm register failed");
            s_pm_id = BTM_PM_SET_ONLY_ID;
            return;
        }
    }

    tBTM_PM_PWR_MD mode = {0};
    mode.mode = (tBTM_PM_MODE)(BTM_PM_MD_ACTIVE | BTM_PM_MD_FORCE);
    BD_ADDR bda_copy;
    memcpy(bda_copy, bda, sizeof(bda_copy));
    if (BTM_SetPowerMode(s_pm_id, bda_copy, &mode) != BTM_SUCCESS) {
        if (s_bt_streaming) {
            ESP_LOGW(TAG, "bt pm set active failed");
        }
    }
}

static void bt_link_policy_disable_sniff(const esp_bd_addr_t bda)
{
    if (!bda) {
        return;
    }

    uint16_t policy = HCI_ENABLE_MASTER_SLAVE_SWITCH;
    BD_ADDR bda_copy;
    memcpy(bda_copy, bda, sizeof(bda_copy));
    tBTM_STATUS st = BTM_SetLinkPolicy(bda_copy, &policy);
    if (st != BTM_SUCCESS) {
        ESP_LOGD(TAG, "bt link policy set failed: %d", st);
    }
}

esp_err_t bt_sink_init(const char *device_name)
{
    esp_err_t err;
    if (s_bt_ready) {
        return ESP_OK;
    }
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

    err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "bt controller init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "bt controller enable failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_err_t tx_err = esp_bredr_tx_power_set(ESP_PWR_LVL_P9, ESP_PWR_LVL_P9);
    if (tx_err != ESP_OK) {
        ESP_LOGW(TAG, "bt tx power set failed: %s", esp_err_to_name(tx_err));
    }

    esp_err_t sleep_err = esp_bt_sleep_disable();
    if (sleep_err != ESP_OK && sleep_err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "bt modem sleep disable failed: %s", esp_err_to_name(sleep_err));
    }

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    bluedroid_cfg.ssp_en = true;
    err = esp_bluedroid_init_with_cfg(&bluedroid_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "bluedroid init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_bluedroid_enable();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "bluedroid enable failed: %s", esp_err_to_name(err));
        return err;
    }

    BTM_SetDefaultLinkPolicy(HCI_ENABLE_MASTER_SLAVE_SWITCH);

    if (!s_gap_cb_registered) {
        err = esp_bt_gap_register_callback(bt_gap_cb);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "gap cb register failed: %s", esp_err_to_name(err));
        } else {
            s_gap_cb_registered = true;
        }
    }

    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    err = esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "gap set iocap failed: %s", esp_err_to_name(err));
    }

    const char *name = (device_name && device_name[0] != '\0') ? device_name : "ClockAudio";
    err = esp_bt_gap_set_device_name(name);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bt set name failed: %s", esp_err_to_name(err));
    }

    bt_app_task_start_up();

#if defined(CONFIG_BT_A2DP_ENABLE) && CONFIG_BT_A2DP_ENABLE
    bt_avrc_init();
#endif

#if defined(CONFIG_BT_A2DP_ENABLE) && CONFIG_BT_A2DP_ENABLE
    if (!s_a2dp_cb_registered) {
        esp_a2d_register_callback(bt_a2d_cb);
        esp_a2d_sink_register_data_callback(bt_app_a2d_data_cb);
        s_a2dp_cb_registered = true;
    }
    err = esp_a2d_sink_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "a2dp sink init failed: %s", esp_err_to_name(err));
        return err;
    }
    esp_a2d_sink_get_delay_value();

    esp_bt_cod_t cod = {0};
    cod.major = ESP_BT_COD_MAJOR_DEV_AV;
    cod.minor = 0;
    cod.service = ESP_BT_COD_SRVC_AUDIO | ESP_BT_COD_SRVC_RENDERING;
    err = esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_ALL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bt cod set failed: %s", esp_err_to_name(err));
    }

    esp_bt_eir_data_t eir = {
        .include_name = true,
        .flag = ESP_BT_EIR_FLAG_GEN_DISC
    };
    err = esp_bt_gap_config_eir_data(&eir);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bt eir config failed: %s", esp_err_to_name(err));
    }
#else
    ESP_LOGW(TAG, "A2DP is disabled in sdkconfig");
#endif

    s_bt_ready = true;
    return ESP_OK;
}

void bt_sink_deinit(void)
{
    if (!s_bt_ready) {
        return;
    }

    if (s_bt_connected) {
        bt_sink_disconnect();
    }

    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

#if defined(CONFIG_BT_A2DP_ENABLE) && CONFIG_BT_A2DP_ENABLE
    bt_avrc_deinit();
    esp_a2d_sink_deinit();
#endif

    bt_app_task_shut_down();

    esp_bluedroid_disable();
    esp_bluedroid_deinit();

    esp_bt_controller_disable();
    esp_bt_controller_deinit();

    bt_sink_reset_state();
}

bool bt_sink_is_ready(void)
{
    return s_bt_ready;
}

esp_err_t bt_sink_set_name(const char *device_name)
{
    if (!s_bt_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    const char *name = (device_name && device_name[0] != '\0') ? device_name : "ClockAudio";
    esp_err_t err = esp_bt_gap_set_device_name(name);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bt set name failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t bt_sink_set_discoverable(bool enabled)
{
    if (!s_bt_ready) {
        s_discoverable_requested = enabled;
        return ESP_ERR_INVALID_STATE;
    }
    s_discoverable_requested = enabled;
    ESP_LOGD(TAG, "bt discoverable=%d", enabled ? 1 : 0);
    esp_bt_connection_mode_t c_mode = enabled ? ESP_BT_CONNECTABLE : ESP_BT_NON_CONNECTABLE;
    esp_bt_discovery_mode_t d_mode = enabled ? ESP_BT_GENERAL_DISCOVERABLE : ESP_BT_NON_DISCOVERABLE;
    esp_err_t err = esp_bt_gap_set_scan_mode(c_mode, d_mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bt scan mode failed: %s", esp_err_to_name(err));
    }

    if (enabled) {
        esp_bt_eir_data_t eir = {
            .include_name = true,
            .flag = ESP_BT_EIR_FLAG_GEN_DISC
        };
        esp_err_t eir_err = esp_bt_gap_config_eir_data(&eir);
        if (eir_err != ESP_OK) {
            ESP_LOGW(TAG, "bt eir refresh failed: %s", esp_err_to_name(eir_err));
        }
    }
    return err;
}

bool bt_sink_is_connected(void)
{
    if (!s_bt_ready) {
        return false;
    }
    return s_bt_connected;
}

static void bt_set_link_supervision_timeout(const esp_bd_addr_t bda)
{
    if (!bda) {
        return;
    }

    const uint16_t timeout = 0xC800; // 0xC800 * 0.625ms ~= 32s
    BD_ADDR bda_copy;
    memcpy(bda_copy, bda, sizeof(bda_copy));
    tBTM_STATUS st = BTM_SetLinkSuperTout(bda_copy, timeout);
    if (st != BTM_SUCCESS) {
        ESP_LOGW(TAG, "bt supervision timeout set failed: %d", st);
    }
}

bool bt_sink_is_playing(void)
{
    if (!s_bt_ready) {
        return false;
    }
    return s_a2d_audio_state == ESP_A2D_AUDIO_STATE_STARTED;
}

bool bt_sink_has_saved_device(void)
{
    if (!s_bt_ready) {
        return false;
    }
    return esp_bt_gap_get_bond_device_num() > 0;
}

esp_err_t bt_sink_try_connect_last(void)
{
    if (!s_bt_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_autoconnect_allowed) {
        ESP_LOGD(TAG, "bt autoconnect blocked");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_bt_connected) {
        return ESP_OK;
    }
#if defined(CONFIG_BT_A2DP_ENABLE) && CONFIG_BT_A2DP_ENABLE
    esp_bd_addr_t bda;
    if (!bt_get_last_bonded(bda)) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = esp_a2d_sink_connect(bda);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bt connect last failed: %s", esp_err_to_name(err));
    }
    return err;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t bt_sink_schedule_connect_last(uint32_t delay_ms)
{
    if (!s_bt_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_bt_connected) {
        return ESP_OK;
    }
    s_autoconnect_allowed = true;
    ESP_LOGD(TAG, "bt autoconnect schedule=%u ms", (unsigned)delay_ms);
    if (!s_autoconnect_timer) {
        const esp_timer_create_args_t args = {
            .callback = bt_autoconnect_timer_cb,
            .name = "bt_autoconnect"
        };
        esp_err_t err = esp_timer_create(&args, &s_autoconnect_timer);
        if (err != ESP_OK) {
            return err;
        }
    } else {
        esp_timer_stop(s_autoconnect_timer);
    }
    return esp_timer_start_once(s_autoconnect_timer, (uint64_t)delay_ms * 1000ULL);
}

esp_err_t bt_sink_disconnect(void)
{
    if (!s_bt_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_bt_connected) {
        return ESP_ERR_INVALID_STATE;
    }
#if defined(CONFIG_BT_A2DP_ENABLE) && CONFIG_BT_A2DP_ENABLE
    esp_bd_addr_t bda;
    memcpy(bda, s_connected_bda, sizeof(bda));
    s_bt_connected = false;
    s_bt_streaming = false;
    bt_last_audio_set(0);
    memset(s_connected_bda, 0, sizeof(s_connected_bda));
    esp_err_t err = esp_a2d_sink_disconnect(bda);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "a2dp disconnect failed: %s", esp_err_to_name(err));
    }
    return err;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t bt_sink_clear_bonds(void)
{
#if defined(CONFIG_BT_ENABLED) && CONFIG_BT_ENABLED
    int dev_num = esp_bt_gap_get_bond_device_num();
    if (dev_num <= 0) {
        return ESP_OK;
    }

    esp_bd_addr_t *dev_list = calloc((size_t)dev_num, sizeof(esp_bd_addr_t));
    if (!dev_list) {
        return ESP_ERR_NO_MEM;
    }

    int list_num = dev_num;
    esp_err_t err = esp_bt_gap_get_bond_device_list(&list_num, dev_list);
    if (err == ESP_OK) {
        for (int i = 0; i < list_num; ++i) {
            esp_bt_gap_remove_bond_device(dev_list[i]);
        }
    }
    free(dev_list);
    return err;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

#else

esp_err_t bt_sink_init(const char *device_name)
{
    (void)device_name;
    ESP_LOGW(TAG, "bluetooth disabled in sdkconfig");
    return ESP_ERR_NOT_SUPPORTED;
}

bool bt_sink_is_ready(void)
{
    return false;
}

esp_err_t bt_sink_set_name(const char *device_name)
{
    (void)device_name;
    ESP_LOGW(TAG, "bluetooth disabled in sdkconfig");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bt_sink_set_discoverable(bool enabled)
{
    (void)enabled;
    ESP_LOGW(TAG, "bluetooth disabled in sdkconfig");
    return ESP_ERR_NOT_SUPPORTED;
}

bool bt_sink_is_connected(void)
{
    return false;
}

bool bt_sink_is_playing(void)
{
    return false;
}

bool bt_sink_has_saved_device(void)
{
    return false;
}

esp_err_t bt_sink_try_connect_last(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bt_sink_schedule_connect_last(uint32_t delay_ms)
{
    (void)delay_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bt_sink_disconnect(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bt_sink_clear_bonds(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif

bool bt_sink_is_streaming(void)
{
    int64_t last_us = bt_last_audio_get();
    if (last_us != 0) {
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_us > BT_STREAM_TIMEOUT_US) {
            bt_last_audio_set(0);
            s_bt_streaming = false;
        }
    }
    return s_bt_streaming;
}

void bt_sink_note_audio_data(void)
{
    bt_last_audio_set(esp_timer_get_time());
    s_bt_streaming = true;
}
