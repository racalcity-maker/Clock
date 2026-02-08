#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RADIO_STATION_MAX 20

typedef struct {
    char wifi_ssid[32];
    char wifi_pass[64];
    char tz[32];
    char bt_name[32];
    uint8_t volume;
    uint8_t eq_low;
    uint8_t eq_high;
    uint8_t display_brightness;
    uint8_t alarm_hour;
    uint8_t alarm_min;
    bool alarm_enabled;
    uint8_t alarm_mode;
    uint8_t alarm_volume;
    uint8_t alarm_tone;
    bool power_save_enabled;
    uint8_t ui_mode;
    uint8_t alarm_repeat;
    bool web_enabled;
    uint8_t radio_station_count;
    uint16_t radio_stations[RADIO_STATION_MAX]; // MHz * 10 (e.g., 1017 => 101.7 MHz)
} app_config_t;

esp_err_t config_store_init(void);
void config_set_defaults(app_config_t *cfg);
void config_store_get(app_config_t *out);
esp_err_t config_store_update(const app_config_t *in);

#ifdef __cplusplus
}
#endif

