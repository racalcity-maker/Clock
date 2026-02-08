#include "config_store.h"

#include "app_control.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"
#include "stdlib.h"
#include "string.h"

#define NVS_NAMESPACE "clock"

static const char *TAG = "config_store";
static app_config_t s_cfg;

static esp_err_t config_load_from_nvs(app_config_t *out)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    config_set_defaults(out);
    size_t size = 0;
    err = nvs_get_blob(nvs, "cfg", NULL, &size);
    if (err != ESP_OK) {
        nvs_close(nvs);
        return err;
    }

    uint8_t *buf = (uint8_t *)malloc(size);
    if (!buf) {
        nvs_close(nvs);
        return ESP_ERR_NO_MEM;
    }
    err = nvs_get_blob(nvs, "cfg", buf, &size);
    if (err == ESP_OK) {
        size_t copy = size < sizeof(*out) ? size : sizeof(*out);
        memcpy(out, buf, copy);
    }
    free(buf);
    nvs_close(nvs);
    return err;
}

static esp_err_t config_save_to_nvs(const app_config_t *cfg)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(nvs, "cfg", cfg, sizeof(*cfg));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

void config_set_defaults(app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->tz, "UTC0", sizeof(cfg->tz) - 1);
    strncpy(cfg->bt_name, "ClockAudio", sizeof(cfg->bt_name) - 1);
    cfg->volume = 15;
    cfg->eq_low = 15;
    cfg->eq_high = 15;
    cfg->display_brightness = 255;
    cfg->alarm_hour = 7;
    cfg->alarm_min = 0;
    cfg->alarm_enabled = false;
    cfg->alarm_mode = 2;
    cfg->alarm_tone = 1;
    cfg->alarm_volume = 1;
    cfg->alarm_repeat = 1;
    cfg->power_save_enabled = false;
    cfg->ui_mode = 0;
    cfg->web_enabled = false;
    cfg->radio_station_count = 0;
    for (int i = 0; i < RADIO_STATION_MAX; ++i) {
        cfg->radio_stations[i] = 0;
    }
}

esp_err_t config_store_init(void)
{
    esp_err_t err = config_load_from_nvs(&s_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "load failed: %s", esp_err_to_name(err));
        config_set_defaults(&s_cfg);
        esp_err_t save_err = config_save_to_nvs(&s_cfg);
        if (save_err != ESP_OK) {
            ESP_LOGE(TAG, "save defaults failed: %s", esp_err_to_name(save_err));
            return save_err;
        }
        return ESP_OK;
    }
    if (s_cfg.volume > APP_VOLUME_MAX) {
        s_cfg.volume = app_volume_steps_from_byte(s_cfg.volume);
    }
    if (s_cfg.eq_low > 30) {
        s_cfg.eq_low = 15;
    }
    if (s_cfg.eq_high > 30) {
        s_cfg.eq_high = 15;
    }
    if (s_cfg.alarm_mode > 2) {
        s_cfg.alarm_mode = 2;
    }
    if (s_cfg.alarm_tone < 1 || s_cfg.alarm_tone > 99) {
        s_cfg.alarm_tone = 1;
    }
    if (s_cfg.alarm_volume < 1 || s_cfg.alarm_volume > APP_VOLUME_MAX) {
        s_cfg.alarm_volume = 1;
    }
    if (s_cfg.alarm_repeat < 1 || s_cfg.alarm_repeat > 5) {
        s_cfg.alarm_repeat = 1;
    }
    if (s_cfg.ui_mode > 3) {
        s_cfg.ui_mode = 0;
    }
    if (s_cfg.web_enabled != true && s_cfg.web_enabled != false) {
        s_cfg.web_enabled = false;
    }
    if (s_cfg.radio_station_count > RADIO_STATION_MAX) {
        s_cfg.radio_station_count = 0;
    }
    for (int i = 0; i < s_cfg.radio_station_count; ++i) {
        uint16_t freq = s_cfg.radio_stations[i];
        if (freq < 870 || freq > 1080) {
            s_cfg.radio_stations[i] = 0;
        }
    }
    return ESP_OK;
}

void config_store_get(app_config_t *out)
{
    if (!out) {
        return;
    }
    *out = s_cfg;
}

esp_err_t config_store_update(const app_config_t *in)
{
    if (!in) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = *in;
    esp_err_t err = config_save_to_nvs(&s_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save failed: %s", esp_err_to_name(err));
    }
    return err;
}

