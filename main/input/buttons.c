#include "buttons.h"

#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static QueueHandle_t s_btn_queue = NULL;
static button_event_cb_t s_btn_cb = NULL;
static TaskHandle_t s_btn_task = NULL;

#define BUTTON_DEBOUNCE_US 30000
#define BUTTON_LONG_US 800000

typedef struct {
    uint32_t gpio;
    int level;
} button_edge_t;

typedef struct {
    bool pressed;
    int64_t last_change_us;
    int64_t pressed_at_us;
} button_state_t;

static button_id_t button_from_gpio(uint32_t gpio)
{
    switch (gpio) {
        case PIN_BTN_1:
            return BUTTON_1;
        case PIN_BTN_2:
            return BUTTON_2;
        case PIN_BTN_3:
            return BUTTON_3;
        case PIN_BTN_4:
            return BUTTON_4;
        default:
            return BUTTON_1;
    }
}

static int button_index_from_gpio(uint32_t gpio)
{
    switch (gpio) {
        case PIN_BTN_1:
            return 0;
        case PIN_BTN_2:
            return 1;
        case PIN_BTN_3:
            return 2;
        case PIN_BTN_4:
            return 3;
        default:
            return -1;
    }
}

static void IRAM_ATTR button_isr_handler(void *arg)
{
    if (!s_btn_queue) {
        return;
    }
    uint32_t gpio_num = (uint32_t)arg;
    button_edge_t edge = {
        .gpio = gpio_num,
        .level = gpio_get_level(gpio_num)
    };
    xQueueSendFromISR(s_btn_queue, &edge, NULL);
}

static void button_task(void *arg)
{
    (void)arg;
    button_edge_t edge = {0};
    button_state_t states[4] = {0};

    while (xQueueReceive(s_btn_queue, &edge, portMAX_DELAY)) {
        int index = button_index_from_gpio(edge.gpio);
        if (index < 0 || index >= 4) {
            continue;
        }

        int64_t now = esp_timer_get_time();
        button_state_t *state = &states[index];
        if (now - state->last_change_us < BUTTON_DEBOUNCE_US) {
            continue;
        }
        state->last_change_us = now;

        if (edge.level == 0 && !state->pressed) {
            state->pressed = true;
            state->pressed_at_us = now;
        } else if (edge.level == 1 && state->pressed) {
            state->pressed = false;
            int64_t duration = now - state->pressed_at_us;
            button_event_t event = (duration >= BUTTON_LONG_US) ? BUTTON_EVENT_LONG : BUTTON_EVENT_SHORT;
            if (s_btn_cb) {
                s_btn_cb(button_from_gpio(edge.gpio), event);
            }
        }
    }
}

void buttons_init(button_event_cb_t cb)
{
    s_btn_cb = cb;
    if (s_btn_queue || s_btn_task) {
        return;
    }

    s_btn_queue = xQueueCreate(10, sizeof(button_edge_t));
    if (!s_btn_queue) {
        return;
    }

    uint64_t pin_mask = 0;
#if PIN_BTN_1 >= 0
    pin_mask |= (1ULL << PIN_BTN_1);
#endif
#if PIN_BTN_2 >= 0
    pin_mask |= (1ULL << PIN_BTN_2);
#endif
#if PIN_BTN_3 >= 0
    pin_mask |= (1ULL << PIN_BTN_3);
#endif
#if PIN_BTN_4 >= 0
    pin_mask |= (1ULL << PIN_BTN_4);
#endif

    gpio_config_t io_conf = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE
    };
    if (gpio_config(&io_conf) != ESP_OK) {
        vQueueDelete(s_btn_queue);
        s_btn_queue = NULL;
        return;
    }

    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        vQueueDelete(s_btn_queue);
        s_btn_queue = NULL;
        return;
    }

    bool add_failed = false;
#if PIN_BTN_1 >= 0
    if (gpio_isr_handler_add(PIN_BTN_1, button_isr_handler, (void *)PIN_BTN_1) != ESP_OK) {
        add_failed = true;
    }
#endif
#if PIN_BTN_2 >= 0
    if (gpio_isr_handler_add(PIN_BTN_2, button_isr_handler, (void *)PIN_BTN_2) != ESP_OK) {
        add_failed = true;
    }
#endif
#if PIN_BTN_3 >= 0
    if (gpio_isr_handler_add(PIN_BTN_3, button_isr_handler, (void *)PIN_BTN_3) != ESP_OK) {
        add_failed = true;
    }
#endif
#if PIN_BTN_4 >= 0
    if (gpio_isr_handler_add(PIN_BTN_4, button_isr_handler, (void *)PIN_BTN_4) != ESP_OK) {
        add_failed = true;
    }
#endif
    if (add_failed) {
#if PIN_BTN_1 >= 0
        gpio_isr_handler_remove(PIN_BTN_1);
#endif
#if PIN_BTN_2 >= 0
        gpio_isr_handler_remove(PIN_BTN_2);
#endif
#if PIN_BTN_3 >= 0
        gpio_isr_handler_remove(PIN_BTN_3);
#endif
#if PIN_BTN_4 >= 0
        gpio_isr_handler_remove(PIN_BTN_4);
#endif
        vQueueDelete(s_btn_queue);
        s_btn_queue = NULL;
        return;
    }

    if (xTaskCreate(button_task, "button_task", 2048, NULL, 10, &s_btn_task) != pdPASS) {
#if PIN_BTN_1 >= 0
        gpio_isr_handler_remove(PIN_BTN_1);
#endif
#if PIN_BTN_2 >= 0
        gpio_isr_handler_remove(PIN_BTN_2);
#endif
#if PIN_BTN_3 >= 0
        gpio_isr_handler_remove(PIN_BTN_3);
#endif
#if PIN_BTN_4 >= 0
        gpio_isr_handler_remove(PIN_BTN_4);
#endif
        vQueueDelete(s_btn_queue);
        s_btn_queue = NULL;
        s_btn_task = NULL;
    }
}

