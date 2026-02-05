#include "encoder.h"

#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "encoder";

#define ENC_DEBOUNCE_US 0
#define ENC_BTN_DEBOUNCE_US 15000
#define ENC_BTN_LONG_US 2000000
#define ENC_LONG_CHECK_MS 20
#define ENC_QUEUE_DEPTH 32
#define ENC_STEPS_PER_DETENT 4
#define ENC_INVERT_DIR 1

typedef enum {
    ENC_EDGE_AB,
    ENC_EDGE_BTN
} enc_edge_type_t;

typedef struct {
    enc_edge_type_t type;
    uint8_t ab_state;
    int level;
} enc_edge_t;

typedef struct {
    bool pressed;
    int64_t last_change_us;
    int64_t pressed_at_us;
} enc_btn_state_t;

static QueueHandle_t s_enc_queue = NULL;
static encoder_event_cb_t s_enc_cb = NULL;
static uint8_t s_last_ab_state = 0;
static int s_accum = 0;
static TaskHandle_t s_enc_task = NULL;

static const int8_t s_dir_table[16] = {
    0, -1,  1,  0,
    1,  0,  0, -1,
   -1,  0,  0,  1,
    0,  1, -1,  0
};

static uint8_t read_ab_state(void)
{
    int a = gpio_get_level(PIN_ENC_A) ? 1 : 0;
    int b = gpio_get_level(PIN_ENC_B) ? 1 : 0;
    return (uint8_t)((a << 1) | b);
}

static void IRAM_ATTR enc_ab_isr(void *arg)
{
    (void)arg;
    enc_edge_t ev = {
        .type = ENC_EDGE_AB,
        .ab_state = read_ab_state(),
        .level = 0
    };
    xQueueSendFromISR(s_enc_queue, &ev, NULL);
}

static void IRAM_ATTR enc_btn_isr(void *arg)
{
    (void)arg;
    enc_edge_t ev = {
        .type = ENC_EDGE_BTN,
        .ab_state = 0,
        .level = gpio_get_level(PIN_ENC_BTN)
    };
    xQueueSendFromISR(s_enc_queue, &ev, NULL);
}

static void encoder_task(void *arg)
{
    (void)arg;
    enc_edge_t ev;
    enc_btn_state_t btn = {0};
    bool long_sent = false;

    while (1) {
        if (xQueueReceive(s_enc_queue, &ev, pdMS_TO_TICKS(ENC_LONG_CHECK_MS)) == pdTRUE) {
            if (ev.type == ENC_EDGE_AB) {
                uint8_t state = ev.ab_state & 0x3;
                uint8_t idx = (uint8_t)((s_last_ab_state << 2) | state);
                int8_t dir = s_dir_table[idx];
                if (ENC_INVERT_DIR) {
                    dir = (int8_t)-dir;
                }
                s_last_ab_state = state;
                if (dir != 0) {
                    s_accum += dir;
                    if (s_accum >= ENC_STEPS_PER_DETENT) {
                        s_accum -= ENC_STEPS_PER_DETENT;
                        if (s_enc_cb) {
                            s_enc_cb(ENC_EVENT_CW);
                        }
                    } else if (s_accum <= -ENC_STEPS_PER_DETENT) {
                        s_accum += ENC_STEPS_PER_DETENT;
                        if (s_enc_cb) {
                            s_enc_cb(ENC_EVENT_CCW);
                        }
                    }
                }
            } else if (ev.type == ENC_EDGE_BTN) {
                int64_t now = esp_timer_get_time();
                if (ev.level == 0) {
                    if (now - btn.last_change_us < ENC_BTN_DEBOUNCE_US) {
                        continue;
                    }
                    btn.last_change_us = now;
                    if (!btn.pressed) {
                        btn.pressed = true;
                        btn.pressed_at_us = now;
                        long_sent = false;
                    }
                } else if (btn.pressed) {
                    btn.last_change_us = now;
                    btn.pressed = false;
                    int64_t duration = now - btn.pressed_at_us;
                    if (duration < ENC_BTN_DEBOUNCE_US) {
                        long_sent = false;
                        continue;
                    }
                    if (!long_sent) {
                        if (duration >= ENC_BTN_LONG_US) {
                            if (s_enc_cb) {
                                s_enc_cb(ENC_EVENT_BTN_LONG);
                            }
                        } else if (s_enc_cb) {
                            s_enc_cb(ENC_EVENT_BTN_SHORT);
                        }
                    }
                    long_sent = false;
                }
            }
        }

        if (btn.pressed && !long_sent) {
            int64_t now = esp_timer_get_time();
            if (now - btn.pressed_at_us >= ENC_BTN_LONG_US) {
                long_sent = true;
                if (s_enc_cb) {
                    s_enc_cb(ENC_EVENT_BTN_LONG);
                }
            }
        }
    }
}

void encoder_init(encoder_event_cb_t cb)
{
    s_enc_cb = cb;

    if (PIN_ENC_A == GPIO_NUM_NC || PIN_ENC_B == GPIO_NUM_NC || PIN_ENC_BTN == GPIO_NUM_NC) {
        ESP_LOGW(TAG, "encoder pins not set");
        return;
    }

    s_enc_queue = xQueueCreate(ENC_QUEUE_DEPTH, sizeof(enc_edge_t));
    if (!s_enc_queue) {
        ESP_LOGE(TAG, "queue create failed");
        return;
    }

    uint64_t ab_mask = 0;
#if PIN_ENC_A >= 0
    ab_mask |= (1ULL << PIN_ENC_A);
#endif
#if PIN_ENC_B >= 0
    ab_mask |= (1ULL << PIN_ENC_B);
#endif
    gpio_config_t ab_conf = {
        .pin_bit_mask = ab_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE
    };
    gpio_config(&ab_conf);

    uint64_t btn_mask = 0;
#if PIN_ENC_BTN >= 0
    btn_mask |= (1ULL << PIN_ENC_BTN);
#endif
    gpio_config_t btn_conf = {
        .pin_bit_mask = btn_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE
    };
    gpio_config(&btn_conf);

    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "isr service failed: %s", esp_err_to_name(err));
        return;
    }

    gpio_isr_handler_add(PIN_ENC_A, enc_ab_isr, NULL);
    gpio_isr_handler_add(PIN_ENC_B, enc_ab_isr, NULL);
    gpio_isr_handler_add(PIN_ENC_BTN, enc_btn_isr, NULL);

    s_last_ab_state = read_ab_state();
    if (xTaskCreate(encoder_task, "encoder_task", 1536, NULL, 9, &s_enc_task) != pdPASS) {
        gpio_isr_handler_remove(PIN_ENC_A);
        gpio_isr_handler_remove(PIN_ENC_B);
        gpio_isr_handler_remove(PIN_ENC_BTN);
        vQueueDelete(s_enc_queue);
        s_enc_queue = NULL;
        s_enc_task = NULL;
    }
}

void encoder_deinit(void)
{
    s_enc_cb = NULL;

    if (PIN_ENC_A != GPIO_NUM_NC) {
        gpio_isr_handler_remove(PIN_ENC_A);
    }
    if (PIN_ENC_B != GPIO_NUM_NC) {
        gpio_isr_handler_remove(PIN_ENC_B);
    }
    if (PIN_ENC_BTN != GPIO_NUM_NC) {
        gpio_isr_handler_remove(PIN_ENC_BTN);
    }

    if (s_enc_task) {
        vTaskDelete(s_enc_task);
        s_enc_task = NULL;
    }
    if (s_enc_queue) {
        vQueueDelete(s_enc_queue);
        s_enc_queue = NULL;
    }
    s_last_ab_state = 0;
    s_accum = 0;
}
