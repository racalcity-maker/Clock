#include "ui_input.h"

#include "adc_keys.h"
#include "encoder.h"
#include "ui_mode_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "ui_input";

#define INPUT_QUEUE_DEPTH 32
#define INPUT_COOLDOWN_ENC_BTN_US 200000
#define INPUT_COOLDOWN_MODE_US 500000
#define INPUT_COOLDOWN_POWER_US 500000
#define INPUT_COOLDOWN_NAV_US 150000
#define INPUT_COOLDOWN_BT_US 300000
#define INPUT_MODE_GUARD_US 600000

typedef enum {
    INPUT_EVT_ENCODER,
    INPUT_EVT_ADC
} input_evt_type_t;

typedef struct {
    input_evt_type_t type;
    int64_t ts_us;
    union {
        encoder_event_t enc;
        struct {
            adc_key_id_t key;
            adc_key_event_t event;
        } adc;
    } data;
} input_evt_t;

static QueueHandle_t s_input_queue = NULL;
static TaskHandle_t s_input_task = NULL;
static int64_t s_last_enc_btn_us = 0;
static int64_t s_last_adc_us[6][2] = {0};
static int64_t s_mode_guard_until_us = 0;

static void input_enqueue_encoder(encoder_event_t event)
{
    if (!s_input_queue) {
        app_request_input_encoder(event);
        return;
    }
    input_evt_t ev = {
        .type = INPUT_EVT_ENCODER,
        .ts_us = esp_timer_get_time()
    };
    ev.data.enc = event;
    (void)xQueueSend(s_input_queue, &ev, 0);
}

static void input_enqueue_adc(adc_key_id_t key, adc_key_event_t event)
{
    if (!s_input_queue) {
        app_request_input_adc(key, event);
        return;
    }
    input_evt_t ev = {
        .type = INPUT_EVT_ADC,
        .ts_us = esp_timer_get_time()
    };
    ev.data.adc.key = key;
    ev.data.adc.event = event;
    (void)xQueueSend(s_input_queue, &ev, 0);
}

static bool input_rate_limit_encoder(const input_evt_t *ev)
{
    if (!ev) {
        return true;
    }
    if (ev->data.enc == ENC_EVENT_CW || ev->data.enc == ENC_EVENT_CCW) {
        return false;
    }
    int64_t now = ev->ts_us;
    if ((now - s_last_enc_btn_us) < INPUT_COOLDOWN_ENC_BTN_US) {
        return true;
    }
    s_last_enc_btn_us = now;
    return false;
}

static int64_t adc_cooldown_us(adc_key_id_t key)
{
    switch (key) {
        case ADC_KEY_MODE:
            return INPUT_COOLDOWN_MODE_US;
        case ADC_KEY_POWER:
            return INPUT_COOLDOWN_POWER_US;
        case ADC_KEY_NEXT:
        case ADC_KEY_PREV:
            return INPUT_COOLDOWN_NAV_US;
        case ADC_KEY_BT:
            return INPUT_COOLDOWN_BT_US;
        default:
            return 0;
    }
}

static bool input_rate_limit_adc(const input_evt_t *ev)
{
    if (!ev) {
        return true;
    }
    if (ev->data.adc.key <= ADC_KEY_NONE || ev->data.adc.key >= 6) {
        return true;
    }
    int key_idx = (int)ev->data.adc.key;
    int evt_idx = (ev->data.adc.event == ADC_KEY_EVENT_LONG) ? 1 : 0;
    int64_t cooldown = adc_cooldown_us(ev->data.adc.key);
    int64_t now = ev->ts_us;
    if (cooldown > 0 && (now - s_last_adc_us[key_idx][evt_idx]) < cooldown) {
        return true;
    }
    s_last_adc_us[key_idx][evt_idx] = now;
    return false;
}

static bool input_mode_guard(const input_evt_t *ev)
{
    if (!ev || ev->type != INPUT_EVT_ADC) {
        return false;
    }
    if (ev->data.adc.key == ADC_KEY_MODE && ev->data.adc.event == ADC_KEY_EVENT_SHORT) {
        int64_t now = ev->ts_us;
        if (now < s_mode_guard_until_us) {
            return true;
        }
        s_mode_guard_until_us = now + INPUT_MODE_GUARD_US;
    }
    return false;
}

static void input_dispatch_task(void *arg)
{
    (void)arg;
    input_evt_t ev;
    while (xQueueReceive(s_input_queue, &ev, portMAX_DELAY) == pdTRUE) {
        if (ev.type == INPUT_EVT_ENCODER) {
            if (input_rate_limit_encoder(&ev)) {
                continue;
            }
            app_request_input_encoder(ev.data.enc);
        } else {
            if (input_rate_limit_adc(&ev) || input_mode_guard(&ev)) {
                continue;
            }
            app_request_input_adc(ev.data.adc.key, ev.data.adc.event);
        }
    }
}

void ui_input_init(encoder_event_cb_t encoder_cb, adc_key_event_cb_t adc_cb)
{
    ui_mode_manager_set_input_handlers(encoder_cb, adc_cb);

    if (!encoder_cb && !adc_cb) {
        return;
    }

    if (!s_input_queue) {
        s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_evt_t));
        if (!s_input_queue) {
            ESP_LOGW(TAG, "input queue create failed, using direct callbacks");
        }
    }

    if (s_input_queue && !s_input_task) {
        if (xTaskCreate(input_dispatch_task, "ui_input", 1536, NULL, 7, &s_input_task) != pdPASS) {
            s_input_task = NULL;
            vQueueDelete(s_input_queue);
            s_input_queue = NULL;
        }
    }

    if (encoder_cb) {
        encoder_init(s_input_queue ? input_enqueue_encoder : encoder_cb);
    }
    if (adc_cb) {
        adc_keys_init(s_input_queue ? input_enqueue_adc : adc_cb);
    }
}

void ui_input_deinit(void)
{
    ui_mode_manager_set_input_handlers(NULL, NULL);

    if (s_input_task) {
        vTaskDelete(s_input_task);
        s_input_task = NULL;
    }
    if (s_input_queue) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
    }

    encoder_deinit();
    adc_keys_deinit();
}
