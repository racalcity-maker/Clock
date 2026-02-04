#include "wifi_ntp.h"

#include "clock_time.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "web_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "string.h"
#include <sys/time.h>
#include <time.h>

static const char *TAG = "wifi_ntp";
static EventGroupHandle_t s_wifi_event_group;
static bool s_wifi_connected = false;
static bool s_sntp_started = false;
static bool s_has_sta = false;
static bool s_sta_netif_created = false;
static bool s_ap_netif_created = false;
static bool s_wifi_driver_inited = false;
static bool s_handlers_registered = false;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static esp_timer_handle_t s_reconnect_timer = NULL;
static TaskHandle_t s_wifi_shutdown_task = NULL;
static bool s_wifi_enabled = false;
static time_t s_last_sync_time = 0;
static bool s_last_sync_valid = false;
static wifi_config_t s_sta_cfg = {0};
static wifi_config_t s_ap_cfg = {0};
static uint8_t s_connect_attempts = 0;
static bool s_web_enabled = false;
static esp_timer_handle_t s_wifi_off_timer = NULL;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_RECONNECT_INTERVAL_US (10 * 1000000LL)
#define WIFI_FAST_RETRY_US (300 * 1000LL)
#define WIFI_MAX_CONNECT_ATTEMPTS 3

static void sntp_sync_cb(struct timeval *tv);
static inline void wifi_clear_connected_bit(void);
static void wifi_shutdown_task(void *arg);
static void wifi_schedule_shutdown(void);
static void wifi_destroy_netifs(void);
static bool wifi_try_connect(const char *reason);
static void wifi_reset_attempts(void);
static void wifi_start_web_if_ready(void);
static void wifi_schedule_disable_after_sync(void);

static inline void wifi_clear_connected_bit(void)
{
    if (s_wifi_event_group) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_shutdown_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(200));
    if (!s_wifi_enabled) {
        if (s_wifi_driver_inited) {
            esp_err_t err = esp_wifi_deinit();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "wifi deinit failed: %s", esp_err_to_name(err));
            } else {
                s_wifi_driver_inited = false;
            }
        }
        wifi_destroy_netifs();
    }
    s_wifi_shutdown_task = NULL;
    vTaskDelete(NULL);
}

static void wifi_schedule_shutdown(void)
{
    if (s_wifi_shutdown_task) {
        return;
    }
    if (xTaskCreate(wifi_shutdown_task, "wifi_shutdown", 3072, NULL, 5, &s_wifi_shutdown_task) != pdPASS) {
        s_wifi_shutdown_task = NULL;
    }
}

static void wifi_start_sntp(void)
{
    if (s_sntp_started) {
        return;
    }
    esp_sntp_set_time_sync_notification_cb(&sntp_sync_cb);
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    s_sntp_started = true;
    ESP_LOGI(TAG, "sntp started");
}

static void sntp_sync_cb(struct timeval *tv)
{
    if (!tv) {
        return;
    }
    s_last_sync_time = tv->tv_sec;
    s_last_sync_valid = true;
    wifi_schedule_disable_after_sync();
}

static void wifi_reconnect_cb(void *arg)
{
    (void)arg;
    if (!s_wifi_enabled || !s_has_sta || s_wifi_connected) {
        return;
    }
    wifi_try_connect("reconnect");
}

static void wifi_fast_retry_cb(void *arg)
{
    (void)arg;
    if (!s_wifi_enabled || !s_has_sta || s_wifi_connected) {
        return;
    }
    wifi_try_connect("fast");
}

static void wifi_start_reconnect_timer(void)
{
    if (!s_has_sta) {
        return;
    }
    if (!s_reconnect_timer) {
        const esp_timer_create_args_t args = {
            .callback = &wifi_reconnect_cb,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "wifi_reconnect"
        };
        esp_timer_create(&args, &s_reconnect_timer);
    }
    if (s_reconnect_timer && !esp_timer_is_active(s_reconnect_timer)) {
        esp_timer_start_periodic(s_reconnect_timer, WIFI_RECONNECT_INTERVAL_US);
    }
}

static void wifi_stop_reconnect_timer(void)
{
    if (s_reconnect_timer && esp_timer_is_active(s_reconnect_timer)) {
        esp_timer_stop(s_reconnect_timer);
    }
}

static void wifi_fast_retry_schedule(void)
{
    if (s_reconnect_timer && esp_timer_is_active(s_reconnect_timer)) {
        esp_timer_stop(s_reconnect_timer);
    }
    if (s_reconnect_timer) {
        esp_timer_delete(s_reconnect_timer);
        s_reconnect_timer = NULL;
    }

    const esp_timer_create_args_t args = {
        .callback = &wifi_fast_retry_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_fast_retry"
    };
    esp_timer_create(&args, &s_reconnect_timer);
    esp_timer_start_once(s_reconnect_timer, WIFI_FAST_RETRY_US);
}

static void wifi_reset_attempts(void)
{
    s_connect_attempts = 0;
}

static void wifi_disable_cb(void *arg)
{
    (void)arg;
    wifi_set_enabled(false);
}

static void wifi_schedule_disable_after_sync(void)
{
    if (s_web_enabled || !s_wifi_enabled || !s_has_sta) {
        return;
    }
    if (!s_wifi_off_timer) {
        const esp_timer_create_args_t args = {
            .callback = &wifi_disable_cb,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "wifi_off"
        };
        esp_timer_create(&args, &s_wifi_off_timer);
    }
    if (s_wifi_off_timer) {
        if (esp_timer_is_active(s_wifi_off_timer)) {
            esp_timer_stop(s_wifi_off_timer);
        }
        esp_timer_start_once(s_wifi_off_timer, 500 * 1000ULL);
    }
}

static void wifi_start_web_if_ready(void)
{
    if (!s_web_enabled || !s_wifi_enabled) {
        return;
    }
    if (!s_has_sta) {
        web_config_start();
        return;
    }
    if (s_wifi_connected) {
        web_config_start();
    }
}

static bool wifi_try_connect(const char *reason)
{
    if (!s_wifi_enabled || !s_has_sta || s_wifi_connected) {
        return false;
    }
    if (s_connect_attempts >= WIFI_MAX_CONNECT_ATTEMPTS) {
        ESP_LOGW(TAG, "wifi attempts exhausted, stopping wifi");
        wifi_set_enabled(false);
        return false;
    }
    s_connect_attempts++;
    ESP_LOGI(TAG, "wifi connect attempt %u/%u%s%s%s",
             (unsigned)s_connect_attempts,
             (unsigned)WIFI_MAX_CONNECT_ATTEMPTS,
             reason ? " (" : "",
             reason ? reason : "",
             reason ? ")" : "");
    esp_wifi_connect();
    return true;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (!s_wifi_enabled && (event_base == WIFI_EVENT || event_base == IP_EVENT)) {
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_set_ps(WIFI_PS_NONE);
        if (s_wifi_enabled) {
            wifi_start_reconnect_timer();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        if (disc) {
            ESP_LOGW(TAG, "wifi disconnected, reason=%d", (int)disc->reason);
            if (disc->reason == WIFI_REASON_AUTH_EXPIRE ||
                disc->reason == WIFI_REASON_ASSOC_FAIL ||
                disc->reason == WIFI_REASON_CONNECTION_FAIL) {
                if (s_wifi_enabled) {
                    wifi_fast_retry_schedule();
                }
                return;
            }
        } else {
            ESP_LOGW(TAG, "wifi disconnected, reason=unknown");
        }
        s_wifi_connected = false;
        wifi_clear_connected_bit();
        if (s_wifi_enabled) {
            wifi_start_reconnect_timer();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_wifi_connected = true;
        wifi_reset_attempts();
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        wifi_stop_reconnect_timer();
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        wifi_start_sntp();
        wifi_start_web_if_ready();
    }
}

static void wifi_build_configs(const app_config_t *cfg)
{
    memset(&s_sta_cfg, 0, sizeof(s_sta_cfg));
    memset(&s_ap_cfg, 0, sizeof(s_ap_cfg));

    s_has_sta = cfg->wifi_ssid[0] != '\0';
    s_web_enabled = cfg->web_enabled;
    if (s_has_sta) {
        strncpy((char *)s_sta_cfg.sta.ssid, cfg->wifi_ssid, sizeof(s_sta_cfg.sta.ssid) - 1);
        strncpy((char *)s_sta_cfg.sta.password, cfg->wifi_pass, sizeof(s_sta_cfg.sta.password) - 1);
        s_sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        s_sta_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        s_sta_cfg.sta.failure_retry_cnt = 3;
        s_sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        s_sta_cfg.sta.pmf_cfg.capable = true;
        s_sta_cfg.sta.pmf_cfg.required = false;
        return;
    }

    strncpy((char *)s_ap_cfg.ap.ssid, "ClockSetup", sizeof(s_ap_cfg.ap.ssid) - 1);
    strncpy((char *)s_ap_cfg.ap.password, "12345678", sizeof(s_ap_cfg.ap.password) - 1);
    s_ap_cfg.ap.ssid_len = 0;
    s_ap_cfg.ap.channel = 1;
    s_ap_cfg.ap.max_connection = 2;
    s_ap_cfg.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    s_ap_cfg.ap.pairwise_cipher = WIFI_CIPHER_TYPE_CCMP;
}

static esp_err_t wifi_driver_start(void)
{
    esp_err_t err;

    if (!s_web_enabled && !s_has_sta) {
        s_wifi_enabled = false;
        return ESP_OK;
    }

    if (s_has_sta) {
        if (!s_sta_netif) {
            s_sta_netif = esp_netif_create_default_wifi_sta();
            s_sta_netif_created = (s_sta_netif != NULL);
        }
    } else {
        if (!s_ap_netif) {
            s_ap_netif = esp_netif_create_default_wifi_ap();
            s_ap_netif_created = (s_ap_netif != NULL);
        }
    }

    if (!s_wifi_driver_inited) {
        wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&wifi_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi init failed: %s", esp_err_to_name(err));
            return err;
        }
        s_wifi_driver_inited = true;
    }

    if (s_has_sta) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi set mode sta failed: %s", esp_err_to_name(err));
            return err;
        }
        err = esp_wifi_set_config(WIFI_IF_STA, &s_sta_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi set config failed: %s", esp_err_to_name(err));
            return err;
        }
    } else {
        err = esp_wifi_set_mode(WIFI_MODE_AP);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi set mode ap failed: %s", esp_err_to_name(err));
            return err;
        }
        err = esp_wifi_set_config(WIFI_IF_AP, &s_ap_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi set ap config failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STOPPED) {
        ESP_LOGE(TAG, "wifi start failed: %s", esp_err_to_name(err));
        return err;
    }

    s_wifi_enabled = true;
    if (s_has_sta) {
        wifi_reset_attempts();
        wifi_try_connect("start");
        wifi_start_reconnect_timer();
    } else {
        wifi_start_web_if_ready();
    }
    return ESP_OK;
}

static void wifi_destroy_netifs(void)
{
    if (s_sta_netif) {
        esp_netif_destroy(s_sta_netif);
        s_sta_netif = NULL;
    }
    if (s_ap_netif) {
        esp_netif_destroy(s_ap_netif);
        s_ap_netif = NULL;
    }
    s_sta_netif_created = false;
    s_ap_netif_created = false;
}

esp_err_t wifi_init(const app_config_t *cfg)
{
    app_config_t local_cfg;
    if (!cfg) {
        config_store_get(&local_cfg);
        cfg = &local_cfg;
    }

    wifi_build_configs(cfg);

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    if (!s_handlers_registered) {
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                            ESP_EVENT_ANY_ID,
                                                            &wifi_event_handler,
                                                            NULL,
                                                            NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                            IP_EVENT_STA_GOT_IP,
                                                            &wifi_event_handler,
                                                            NULL,
                                                            NULL));
        s_handlers_registered = true;
    }

    ESP_ERROR_CHECK(wifi_driver_start());
    if (s_has_sta) {
        ESP_LOGI(TAG, "wifi sta start");
    } else {
        ESP_LOGI(TAG, "wifi ap start");
    }

    clock_time_set_timezone(cfg->tz);
    return ESP_OK;
}

bool wifi_is_connected(void)
{
    return s_wifi_connected;
}

void wifi_request_time_sync(void)
{
    wifi_start_sntp();
}

void wifi_set_enabled(bool enabled)
{
    if (!enabled) {
        wifi_stop_reconnect_timer();
        if (s_reconnect_timer) {
            esp_timer_delete(s_reconnect_timer);
            s_reconnect_timer = NULL;
        }
        if (s_sntp_started) {
            esp_sntp_stop();
            s_sntp_started = false;
        }
        s_wifi_enabled = false;
        web_config_stop();
        if (s_wifi_driver_inited) {
            esp_err_t err = esp_wifi_stop();
            if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
                ESP_LOGW(TAG, "wifi stop failed: %s", esp_err_to_name(err));
            }
        }
        wifi_schedule_shutdown();
        s_wifi_connected = false;
        wifi_clear_connected_bit();
        return;
    }

    if (enabled == s_wifi_enabled) {
        return;
    }

    wifi_driver_start();
}

bool wifi_is_enabled(void)
{
    return s_wifi_enabled;
}

void wifi_set_web_enabled(bool enabled)
{
    s_web_enabled = enabled;
    if (!s_web_enabled) {
        web_config_stop();
        if (s_wifi_enabled && s_has_sta) {
            wifi_request_time_sync();
            wifi_schedule_disable_after_sync();
        } else if (s_wifi_enabled && !s_has_sta) {
            wifi_set_enabled(false);
        }
        return;
    }
    if (!s_wifi_enabled) {
        wifi_set_enabled(true);
    }
    wifi_start_web_if_ready();
}

bool wifi_wait_for_shutdown(uint32_t timeout_ms)
{
    int64_t start_us = esp_timer_get_time();
    while (s_wifi_shutdown_task) {
        if ((esp_timer_get_time() - start_us) >= ((int64_t)timeout_ms * 1000)) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return true;
}

bool wifi_is_ap_mode(void)
{
    return !s_has_sta;
}

bool wifi_get_last_sync_time(time_t *out)
{
    if (!s_last_sync_valid) {
        return false;
    }
    if (out) {
        *out = s_last_sync_time;
    }
    return true;
}

esp_err_t wifi_update_credentials(const char *ssid, const char *password)
{
    if (!ssid) {
        ssid = "";
    }
    if (!password) {
        password = "";
    }

    app_config_t cfg = {0};
    cfg.web_enabled = s_web_enabled;
    strncpy(cfg.wifi_ssid, ssid, sizeof(cfg.wifi_ssid) - 1);
    strncpy(cfg.wifi_pass, password, sizeof(cfg.wifi_pass) - 1);
    wifi_build_configs(&cfg);

    if (!s_wifi_enabled) {
        ESP_LOGI(TAG, "wifi credentials updated (stored)");
        return ESP_OK;
    }

    wifi_stop_reconnect_timer();
    if (s_wifi_driver_inited) {
        esp_err_t stop_err = esp_wifi_stop();
        if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_STARTED) {
            ESP_LOGE(TAG, "wifi stop failed: %s", esp_err_to_name(stop_err));
            return stop_err;
        }
    }
    s_wifi_connected = false;
    wifi_clear_connected_bit();

    esp_err_t err = wifi_driver_start();
    if (err != ESP_OK) {
        return err;
    }
    if (s_has_sta) {
        ESP_LOGI(TAG, "wifi credentials updated (sta)");
    } else {
        ESP_LOGI(TAG, "wifi credentials cleared (ap)");
    }
    return ESP_OK;
}

