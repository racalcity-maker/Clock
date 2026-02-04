#include "led_indicator.h"

#include "board_pins.h"
#include "display_74hc595.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "led_strip_encoder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define RMT_LED_STRIP_RESOLUTION_HZ 10000000
#define LED_INDICATOR_PIXELS 2
#define LED_SECONDS_COLOR_R 255
#define LED_SECONDS_COLOR_G 80
#define LED_SECONDS_COLOR_B 0
#define LED_SECONDS_BLINK_MS 500

static const char *TAG = "led_indicator";
static rmt_channel_handle_t s_led_chan = NULL;
static rmt_encoder_handle_t s_led_encoder = NULL;
static bool s_led_ready = false;
static uint8_t s_pixels[LED_INDICATOR_PIXELS * 3] = {0};
static uint8_t s_status_rgb[3] = {0};
static uint8_t s_seconds_rgb[3] = {LED_SECONDS_COLOR_R, LED_SECONDS_COLOR_G, LED_SECONDS_COLOR_B};
static bool s_seconds_enabled = true;
static bool s_seconds_on = false;
static SemaphoreHandle_t s_led_mutex = NULL;
static TaskHandle_t s_led_task = NULL;

static uint8_t led_indicator_scale(uint8_t value, uint8_t brightness)
{
    if (brightness >= 255 || value == 0) {
        return value;
    }
    return (uint8_t)(((uint16_t)value * brightness + 127) / 255);
}

static void led_indicator_set_pixel(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
    if (index >= LED_INDICATOR_PIXELS) {
        return;
    }
    size_t base = (size_t)index * 3;
    s_pixels[base + 0] = r;
    s_pixels[base + 1] = g;
    s_pixels[base + 2] = b;
}

static void led_indicator_apply_locked(void)
{
    bool status_active = (s_status_rgb[0] | s_status_rgb[1] | s_status_rgb[2]) != 0;
    uint8_t brightness = display_get_brightness();
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    if (status_active) {
        r = s_status_rgb[0];
        g = s_status_rgb[1];
        b = s_status_rgb[2];
    } else if (s_seconds_enabled && s_seconds_on) {
        r = s_seconds_rgb[0];
        g = s_seconds_rgb[1];
        b = s_seconds_rgb[2];
    }

    r = led_indicator_scale(r, brightness);
    g = led_indicator_scale(g, brightness);
    b = led_indicator_scale(b, brightness);

    for (uint8_t i = 0; i < LED_INDICATOR_PIXELS; ++i) {
        led_indicator_set_pixel(i, r, g, b);
    }

    rmt_transmit_config_t tx_config = {
        .loop_count = 0
    };
    rmt_transmit(s_led_chan, s_led_encoder, s_pixels, sizeof(s_pixels), &tx_config);
    rmt_tx_wait_all_done(s_led_chan, portMAX_DELAY);
}

static void led_indicator_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(LED_SECONDS_BLINK_MS));
        s_seconds_on = !s_seconds_on;
        if (!s_led_ready || !s_led_mutex) {
            continue;
        }
        if (xSemaphoreTake(s_led_mutex, portMAX_DELAY) == pdTRUE) {
            led_indicator_apply_locked();
            xSemaphoreGive(s_led_mutex);
        }
    }
}

esp_err_t led_indicator_init(void)
{
    if (s_led_ready) {
        return ESP_OK;
    }

    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = PIN_LED_STRIP,
        .mem_block_symbols = 64,
        .resolution_hz = RMT_LED_STRIP_RESOLUTION_HZ,
        .trans_queue_depth = 4
    };

    esp_err_t err = rmt_new_tx_channel(&tx_chan_config, &s_led_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt channel failed: %s", esp_err_to_name(err));
        return err;
    }

    led_strip_encoder_config_t encoder_config = {
        .resolution = RMT_LED_STRIP_RESOLUTION_HZ
    };
    err = rmt_new_led_strip_encoder(&encoder_config, &s_led_encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led encoder failed: %s", esp_err_to_name(err));
        return err;
    }

    err = rmt_enable(s_led_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt enable failed: %s", esp_err_to_name(err));
        return err;
    }

    if (!s_led_mutex) {
        s_led_mutex = xSemaphoreCreateMutex();
    }

    s_led_ready = true;
    led_indicator_set_rgb(0, 0, 0);
    if (!s_led_task) {
        if (xTaskCreate(led_indicator_task, "led_indicator", 2048, NULL, 4, &s_led_task) != pdPASS) {
            s_led_task = NULL;
            s_led_ready = false;
            if (s_led_mutex) {
                vSemaphoreDelete(s_led_mutex);
                s_led_mutex = NULL;
            }
            rmt_disable(s_led_chan);
            rmt_del_channel(s_led_chan);
            s_led_chan = NULL;
            rmt_del_encoder(s_led_encoder);
            s_led_encoder = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

void led_indicator_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    s_status_rgb[0] = r;
    s_status_rgb[1] = g;
    s_status_rgb[2] = b;
    if (!s_led_ready || !s_led_mutex) {
        return;
    }
    if (xSemaphoreTake(s_led_mutex, portMAX_DELAY) == pdTRUE) {
        led_indicator_apply_locked();
        xSemaphoreGive(s_led_mutex);
    }
}

void led_indicator_set_seconds_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    s_seconds_rgb[0] = r;
    s_seconds_rgb[1] = g;
    s_seconds_rgb[2] = b;
    if (!s_led_ready || !s_led_mutex) {
        return;
    }
    if (xSemaphoreTake(s_led_mutex, portMAX_DELAY) == pdTRUE) {
        led_indicator_apply_locked();
        xSemaphoreGive(s_led_mutex);
    }
}

void led_indicator_set_seconds_enabled(bool enabled)
{
    s_seconds_enabled = enabled;
    if (!s_led_ready || !s_led_mutex) {
        return;
    }
    if (xSemaphoreTake(s_led_mutex, portMAX_DELAY) == pdTRUE) {
        led_indicator_apply_locked();
        xSemaphoreGive(s_led_mutex);
    }
}
