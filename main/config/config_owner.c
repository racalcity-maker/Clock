#include "config_owner.h"

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "config_owner";

typedef enum {
    CFG_CMD_UPDATE
} cfg_cmd_type_t;

typedef struct {
    cfg_cmd_type_t type;
    app_config_t cfg;
} cfg_cmd_t;

static QueueHandle_t s_cfg_queue = NULL;
static TaskHandle_t s_cfg_task = NULL;
static app_config_t *s_cfg_ptr = NULL;
static SemaphoreHandle_t s_cfg_mutex = NULL;

static void config_lock(void)
{
    if (s_cfg_mutex) {
        xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    }
}

static void config_unlock(void)
{
    if (s_cfg_mutex) {
        xSemaphoreGive(s_cfg_mutex);
    }
}

static void cfg_owner_task(void *arg)
{
    (void)arg;
    cfg_cmd_t cmd;
    while (xQueueReceive(s_cfg_queue, &cmd, portMAX_DELAY) == pdTRUE) {
        if (cmd.type == CFG_CMD_UPDATE) {
            if (s_cfg_ptr) {
                config_lock();
                *s_cfg_ptr = cmd.cfg;
                config_unlock();
            }
            esp_err_t err = config_store_update(&cmd.cfg);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "config save failed: %s", esp_err_to_name(err));
            }
        }
    }
}

void config_owner_init(app_config_t *cfg)
{
    s_cfg_ptr = cfg;
    if (!s_cfg_mutex) {
        s_cfg_mutex = xSemaphoreCreateMutex();
    }
}

void config_owner_start(void)
{
    if (s_cfg_queue) {
        return;
    }
    s_cfg_queue = xQueueCreate(6, sizeof(cfg_cmd_t));
    if (!s_cfg_queue) {
        ESP_LOGW(TAG, "config queue create failed");
        return;
    }
    if (xTaskCreate(cfg_owner_task, "cfg_owner", 4096, NULL, 5, &s_cfg_task) != pdPASS) {
        ESP_LOGW(TAG, "config task create failed");
        vQueueDelete(s_cfg_queue);
        s_cfg_queue = NULL;
        s_cfg_task = NULL;
    }
}

bool config_owner_request_update(const app_config_t *cfg)
{
    if (!cfg) {
        return false;
    }
    if (!s_cfg_queue) {
        if (s_cfg_ptr) {
            config_lock();
            *s_cfg_ptr = *cfg;
            config_unlock();
        }
        return (config_store_update(cfg) == ESP_OK);
    }
    cfg_cmd_t cmd = {
        .type = CFG_CMD_UPDATE,
        .cfg = *cfg
    };
    if (xQueueSend(s_cfg_queue, &cmd, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "config queue full");
        return false;
    }
    return true;
}
