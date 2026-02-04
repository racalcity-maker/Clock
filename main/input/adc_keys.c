#include "adc_keys.h"

#include "board_pins.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "adc_keys";

#define ADC_KEYS_POLL_MS 20
#define ADC_KEYS_STABLE_SAMPLES 3
#define ADC_KEYS_LONG_US 2000000

// Adjust these ranges to match your resistor ladder (raw 0-4095 at 11dB).
#define ADC_KEY_POWER_MIN 0
#define ADC_KEY_POWER_MAX 300
#define ADC_KEY_MODE_MIN 500
#define ADC_KEY_MODE_MAX 900
#define ADC_KEY_NEXT_MIN 1100
#define ADC_KEY_NEXT_MAX 1500
#define ADC_KEY_PREV_MIN 1700
#define ADC_KEY_PREV_MAX 2200
#define ADC_KEY_BT_MIN 3000
#define ADC_KEY_BT_MAX 3600
#define ADC_KEY_NONE_MIN 3800

typedef struct {
    adc_key_id_t key;
    int min;
    int max;
} adc_key_range_t;

static const adc_key_range_t s_ranges[] = {
    {ADC_KEY_POWER, ADC_KEY_POWER_MIN, ADC_KEY_POWER_MAX},
    {ADC_KEY_MODE, ADC_KEY_MODE_MIN, ADC_KEY_MODE_MAX},
    {ADC_KEY_NEXT, ADC_KEY_NEXT_MIN, ADC_KEY_NEXT_MAX},
    {ADC_KEY_PREV, ADC_KEY_PREV_MIN, ADC_KEY_PREV_MAX},
    {ADC_KEY_BT, ADC_KEY_BT_MIN, ADC_KEY_BT_MAX}
};

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_key_event_cb_t s_cb = NULL;
static TaskHandle_t s_adc_task = NULL;

static adc_key_id_t adc_keys_classify(int raw)
{
    if (raw < 0) {
        return ADC_KEY_NONE;
    }
    if (raw >= ADC_KEY_NONE_MIN) {
        return ADC_KEY_NONE;
    }
    for (size_t i = 0; i < sizeof(s_ranges) / sizeof(s_ranges[0]); ++i) {
        if (raw >= s_ranges[i].min && raw <= s_ranges[i].max) {
            return s_ranges[i].key;
        }
    }
    return ADC_KEY_NONE;
}

static void adc_keys_task(void *arg)
{
    (void)arg;
    adc_key_id_t stable_key = ADC_KEY_NONE;
    adc_key_id_t last_key = ADC_KEY_NONE;
    int stable_count = 0;
    int64_t pressed_at = 0;
    bool pressed = false;

    while (1) {
        int raw = 0;
        if (adc_oneshot_read(s_adc_handle, ADC_KEYS_CHANNEL, &raw) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(ADC_KEYS_POLL_MS));
            continue;
        }

        adc_key_id_t key = adc_keys_classify(raw);
        if (key == last_key) {
            if (stable_count < ADC_KEYS_STABLE_SAMPLES) {
                stable_count++;
            }
        } else {
            stable_count = 0;
            last_key = key;
        }

        if (stable_count >= ADC_KEYS_STABLE_SAMPLES && key != stable_key) {
            adc_key_id_t prev_key = stable_key;
            stable_key = key;
            if (stable_key != ADC_KEY_NONE) {
                pressed = true;
                pressed_at = esp_timer_get_time();
            } else if (pressed) {
                int64_t duration = esp_timer_get_time() - pressed_at;
                adc_key_event_t event = (duration >= ADC_KEYS_LONG_US) ? ADC_KEY_EVENT_LONG : ADC_KEY_EVENT_SHORT;
                if (s_cb && prev_key != ADC_KEY_NONE) {
                    s_cb(prev_key, event);
                }
                pressed = false;
                pressed_at = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(ADC_KEYS_POLL_MS));
    }
}

void adc_keys_init(adc_key_event_cb_t cb)
{
    s_cb = cb;

    if (PIN_ADC_KEYS == GPIO_NUM_NC) {
        ESP_LOGW(TAG, "ADC keys pin not set");
        return;
    }

    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE
    };
    if (adc_oneshot_new_unit(&init_cfg, &s_adc_handle) != ESP_OK) {
        ESP_LOGE(TAG, "adc oneshot init failed");
        return;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12
    };
    if (adc_oneshot_config_channel(s_adc_handle, ADC_KEYS_CHANNEL, &chan_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "adc channel config failed");
        return;
    }

    if (xTaskCreate(adc_keys_task, "adc_keys", 2048, NULL, 6, &s_adc_task) != pdPASS) {
        adc_oneshot_del_unit(s_adc_handle);
        s_adc_handle = NULL;
        s_adc_task = NULL;
    }
}

void adc_keys_deinit(void)
{
    s_cb = NULL;
    if (s_adc_task) {
        vTaskDelete(s_adc_task);
        s_adc_task = NULL;
    }
    if (s_adc_handle) {
        adc_oneshot_del_unit(s_adc_handle);
        s_adc_handle = NULL;
    }
}
