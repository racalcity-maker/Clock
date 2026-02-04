#include "display_74hc595.h"

#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#if DISPLAY_USE_SPI
static const char *TAG = "display_74hc595";
#endif

#define DISPLAY_PWM_STEPS 16
#define DISPLAY_REFRESH_HZ 12000
#define DISPLAY_TIMER_PERIOD_US (1000000 / DISPLAY_REFRESH_HZ)

#ifndef DISPLAY_SEGMENT_ACTIVE_LOW
#define DISPLAY_SEGMENT_ACTIVE_LOW 0
#endif

#if DISPLAY_USE_SPI
#ifndef DISPLAY_SPI_USE_CS_LATCH
#define DISPLAY_SPI_USE_CS_LATCH 1
#endif
#endif

static const uint8_t s_digit_map[10] = {
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F,             // 0
    SEG_B | SEG_C,                                             // 1
    SEG_A | SEG_B | SEG_D | SEG_E | SEG_G,                     // 2
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_G,                     // 3
    SEG_B | SEG_C | SEG_F | SEG_G,                             // 4
    SEG_A | SEG_C | SEG_D | SEG_F | SEG_G,                     // 5
    SEG_A | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G,             // 6
    SEG_A | SEG_B | SEG_C,                                     // 7
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G,      // 8
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_F | SEG_G              // 9
};

static uint8_t display_encode_char(char c)
{
    if (c >= '0' && c <= '9') {
        return s_digit_map[c - '0'];
    }

    switch (c) {
        case 'A':
        case 'a':
            return SEG_A | SEG_B | SEG_C | SEG_E | SEG_F | SEG_G;
        case 'B':
        case 'b':
            return SEG_C | SEG_D | SEG_E | SEG_F | SEG_G;
        case 'C':
        case 'c':
            return SEG_A | SEG_D | SEG_E | SEG_F;
        case 'd':
            return SEG_B | SEG_C | SEG_D | SEG_E | SEG_G;
        case 'D':
            return SEG_B | SEG_C | SEG_D | SEG_E | SEG_G;
        case 'E':
        case 'e':
            return SEG_A | SEG_D | SEG_E | SEG_F | SEG_G;
        case 'F':
        case 'f':
            return SEG_A | SEG_E | SEG_F | SEG_G;
        case 'G':
        case 'g':
            return SEG_A | SEG_C | SEG_D | SEG_E | SEG_F;
        case 'H':
        case 'h':
            return SEG_B | SEG_C | SEG_E | SEG_F | SEG_G;
        case 'I':
        case 'i':
            return SEG_B | SEG_C;
        case 'J':
        case 'j':
            return SEG_B | SEG_C | SEG_D | SEG_E;
        case 'L':
        case 'l':
            return SEG_D | SEG_E | SEG_F;
        case 'N':
        case 'n':
            return SEG_C | SEG_E | SEG_G;
        case 'O':
        case 'o':
            return SEG_C | SEG_D | SEG_E | SEG_G;
        case 'Q':
        case 'q':
            return SEG_A | SEG_B | SEG_C | SEG_F | SEG_G;
        case 'P':
        case 'p':
            return SEG_A | SEG_B | SEG_E | SEG_F | SEG_G;
        case 'R':
        case 'r':
            return SEG_E | SEG_G;
        case 'S':
        case 's':
            return SEG_A | SEG_C | SEG_D | SEG_F | SEG_G;
        case 'T':
        case 't':
            return SEG_D | SEG_E | SEG_F | SEG_G;
        case 'U':
        case 'u':
            return SEG_B | SEG_C | SEG_D | SEG_E | SEG_F;
        case 'V':
        case 'v':
            return SEG_C | SEG_D | SEG_E;
        case 'Y':
        case 'y':
            return SEG_B | SEG_C | SEG_D | SEG_F | SEG_G;
        case 'X':
        case 'x':
            return SEG_B | SEG_C | SEG_E | SEG_F | SEG_G;
        case '-':
            return SEG_G;
        case ' ':
        default:
            return 0;
    }
}

static volatile uint32_t s_display_packed = 0;
static volatile uint8_t s_pwm_threshold = DISPLAY_PWM_STEPS;
static uint8_t s_pwm_phase = 0;
static uint8_t s_brightness = 255;
static esp_timer_handle_t s_refresh_timer = NULL;
static bool s_use_hw_pwm = false;
static uint32_t s_ledc_max_duty = 0;
static bool s_static_mode = false;
static bool s_refresh_paused = false;
static bool s_refresh_was_static = false;
#if DISPLAY_USE_SPI
static spi_device_handle_t s_spi = NULL;
static bool s_spi_ready = false;
static spi_host_device_t s_spi_host = SPI3_HOST;
static SemaphoreHandle_t s_spi_mutex = NULL;
#endif

static void sr_write_bit(bool level)
{
    gpio_set_level(PIN_SR_DATA, level);
    gpio_set_level(PIN_SR_CLK, 1);
    gpio_set_level(PIN_SR_CLK, 0);
}

static void sr_latch(void)
{
    gpio_set_level(PIN_SR_LATCH, 1);
    gpio_set_level(PIN_SR_LATCH, 0);
}

#if DISPLAY_USE_SPI
static spi_host_device_t display_spi_select_host(void)
{
    spi_host_device_t host = DISPLAY_SPI_HOST;
    if (host == SPI2_HOST) {
        ESP_LOGW(TAG, "DISPLAY_SPI_HOST=SPI2_HOST, forcing SPI3_HOST");
        host = SPI3_HOST;
    }
    return host;
}

static void display_init_spi(void)
{
    s_spi_host = display_spi_select_host();

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_SR_DATA,
        .miso_io_num = -1,
        .sclk_io_num = PIN_SR_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4,
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_MOSI | SPICOMMON_BUSFLAG_SCLK
    };

    int dma_chan = 0;
#if DISPLAY_SPI_USE_DMA
    dma_chan = SPI_DMA_CH_AUTO;
#endif
    esp_err_t err = spi_bus_initialize(s_spi_host, &bus_cfg, dma_chan);
    if (err == ESP_ERR_INVALID_STATE) {
        err = ESP_OK;
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "spi bus init failed: %s", esp_err_to_name(err));
        s_spi_ready = false;
        return;
    }

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = DISPLAY_SPI_CLOCK_HZ,
        .mode = DISPLAY_SPI_MODE,
        .spics_io_num = DISPLAY_SPI_USE_CS_LATCH ? PIN_SR_LATCH : -1,
        .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX
    };
#if DISPLAY_SPI_LSB_FIRST
    dev_cfg.flags |= SPI_DEVICE_TXBIT_LSBFIRST;
#endif

    err = spi_bus_add_device(s_spi_host, &dev_cfg, &s_spi);
    if (err == ESP_OK) {
        s_spi_ready = true;
        if (!s_spi_mutex) {
            s_spi_mutex = xSemaphoreCreateMutex();
        }
    } else {
        ESP_LOGW(TAG, "spi display init failed: %s", esp_err_to_name(err));
        s_spi_ready = false;
    }
}
#endif

static void sr_write_32(uint32_t value)
{
#if DISPLAY_USE_SPI
    if (s_spi_ready && !s_refresh_paused) {
        uint8_t tx[4] = {
            (uint8_t)((value >> 24) & 0xFF),
            (uint8_t)((value >> 16) & 0xFF),
            (uint8_t)((value >> 8) & 0xFF),
            (uint8_t)(value & 0xFF)
        };
        spi_transaction_t t = {0};
        t.length = 32;
        t.tx_buffer = tx;
#if !DISPLAY_SPI_USE_CS_LATCH
        gpio_set_level(PIN_SR_LATCH, 0);
#endif
        if (s_spi_mutex) {
            xSemaphoreTake(s_spi_mutex, portMAX_DELAY);
        }
        esp_err_t err = spi_device_polling_transmit(s_spi, &t);
        if (s_spi_mutex) {
            xSemaphoreGive(s_spi_mutex);
        }
        if (err == ESP_OK) {
#if !DISPLAY_SPI_USE_CS_LATCH
            sr_latch();
#endif
            return;
        }
        ESP_LOGW(TAG, "spi transmit failed: %s", esp_err_to_name(err));
    }
#endif
    for (int i = 31; i >= 0; --i) {
        sr_write_bit((value >> i) & 0x1);
    }
    sr_latch();
}

static void display_refresh_cb(void *arg)
{
    (void)arg;

    if (s_static_mode) {
        return;
    }

    uint32_t packed = s_display_packed;
    if (s_use_hw_pwm) {
        sr_write_32(packed);
        return;
    }

    uint8_t threshold = s_pwm_threshold;
    s_pwm_phase++;
    if (s_pwm_phase >= DISPLAY_PWM_STEPS) {
        s_pwm_phase = 0;
    }

    if (threshold == 0) {
        sr_write_32(0);
        return;
    }

    if (threshold >= DISPLAY_PWM_STEPS || s_pwm_phase < threshold) {
        sr_write_32(packed);
    } else {
        sr_write_32(0);
    }
}

static void display_init_hw_pwm(void)
{
    if (PIN_SR_OE == GPIO_NUM_NC) {
        return;
    }

    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timer_cfg);

    ledc_channel_config_t ch_cfg = {
        .gpio_num = PIN_SR_OE,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0
    };
    ledc_channel_config(&ch_cfg);

    s_ledc_max_duty = (1U << LEDC_TIMER_8_BIT) - 1;
    s_use_hw_pwm = true;
}

void display_init(void)
{
#if DISPLAY_USE_SPI
    display_init_spi();
#endif

    uint64_t pin_mask = 0;
#if DISPLAY_USE_SPI
    if (!s_spi_ready || !DISPLAY_SPI_USE_CS_LATCH) {
        pin_mask |= (1ULL << PIN_SR_LATCH);
    }
    if (!s_spi_ready) {
        pin_mask |= (1ULL << PIN_SR_DATA) | (1ULL << PIN_SR_CLK);
    }
#else
    pin_mask |= (1ULL << PIN_SR_LATCH) | (1ULL << PIN_SR_DATA) | (1ULL << PIN_SR_CLK);
#endif
    gpio_config_t io_conf = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (pin_mask != 0) {
        gpio_config(&io_conf);
    }

    if (pin_mask & (1ULL << PIN_SR_LATCH)) {
        gpio_set_level(PIN_SR_LATCH, 0);
    }
    if (pin_mask & (1ULL << PIN_SR_DATA)) {
        gpio_set_level(PIN_SR_DATA, 0);
    }
    if (pin_mask & (1ULL << PIN_SR_CLK)) {
        gpio_set_level(PIN_SR_CLK, 0);
    }

    display_init_hw_pwm();

    if (!s_use_hw_pwm) {
        const esp_timer_create_args_t args = {
            .callback = &display_refresh_cb,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "display_refresh"
        };
        esp_timer_create(&args, &s_refresh_timer);
        esp_timer_start_periodic(s_refresh_timer, DISPLAY_TIMER_PERIOD_US);
    }

    display_set_brightness(255);
    sr_write_32(0);
}

void display_set_digits(const uint8_t digits[4], bool colon)
{
    uint8_t segs[4] = {0};
    for (int i = 0; i < 4; ++i) {
        uint8_t d = digits[i] % 10;
        segs[i] = s_digit_map[d];
    }
    display_set_segments(segs, colon);
}

void display_set_time(uint8_t hours, uint8_t minutes, bool colon)
{
    uint8_t digits[4] = {
        (uint8_t)(hours / 10),
        (uint8_t)(hours % 10),
        (uint8_t)(minutes / 10),
        (uint8_t)(minutes % 10)
    };
    display_set_digits(digits, colon);
}

void display_set_brightness(uint8_t level)
{
    s_brightness = level;

    if (s_use_hw_pwm) {
        uint32_t duty = s_ledc_max_duty - ((uint32_t)level * s_ledc_max_duty) / 255;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        if (level == 0) {
            sr_write_32(0);
        } else if (s_static_mode) {
            sr_write_32(s_display_packed);
        }
        return;
    }

    uint32_t scaled = ((uint32_t)level * DISPLAY_PWM_STEPS + 254) / 255;
    if (level > 0 && scaled == 0) {
        scaled = 1;
    }
    if (scaled > DISPLAY_PWM_STEPS) {
        scaled = DISPLAY_PWM_STEPS;
    }
    s_pwm_threshold = (uint8_t)scaled;
    if (level == 0 && s_static_mode) {
        sr_write_32(0);
    } else if (s_static_mode) {
        sr_write_32(s_display_packed);
    }
}

uint8_t display_get_brightness(void)
{
    return s_brightness;
}

void display_set_text(const char text[4], bool colon)
{
    uint8_t segs[4] = {0};
    for (int i = 0; i < 4; ++i) {
        segs[i] = display_encode_char(text[i]);
    }
    display_set_segments(segs, colon);
}

void display_set_segments(const uint8_t segs_in[4], bool colon)
{
    uint8_t segs[4] = {0};
    if (segs_in) {
        for (int i = 0; i < 4; ++i) {
            segs[i] = segs_in[i];
        }
    }

    // Q0 can be used for colon/DP if wired.
    if (colon) {
        segs[1] |= SEG_DP;
    }

#if DISPLAY_SEGMENT_ACTIVE_LOW
    for (int i = 0; i < 4; ++i) {
        segs[i] = (uint8_t)~segs[i];
    }
#endif

    // First register is the first digit: it receives the last byte shifted.
    uint32_t packed = ((uint32_t)segs[3] << 24) |
                      ((uint32_t)segs[2] << 16) |
                      ((uint32_t)segs[1] << 8) |
                      ((uint32_t)segs[0] << 0);

    s_display_packed = packed;
    if (s_use_hw_pwm) {
        sr_write_32(packed);
    } else if (s_static_mode) {
        sr_write_32(packed);
    }
}

void display_set_static(bool enable)
{
    s_static_mode = enable;
    if (s_use_hw_pwm) {
        return;
    }
    if (!s_refresh_timer) {
        return;
    }
    if (enable) {
        if (esp_timer_is_active(s_refresh_timer)) {
            esp_timer_stop(s_refresh_timer);
        }
        sr_write_32(s_display_packed);
    } else {
        if (!esp_timer_is_active(s_refresh_timer)) {
            esp_timer_start_periodic(s_refresh_timer, DISPLAY_TIMER_PERIOD_US);
        }
    }
}

void display_pause_refresh(bool pause)
{
    if (pause) {
        if (s_refresh_paused) {
            return;
        }
        s_refresh_paused = true;
        s_refresh_was_static = s_static_mode;
        display_set_static(true);
#if DISPLAY_USE_SPI
        if (s_spi_mutex) {
            xSemaphoreTake(s_spi_mutex, portMAX_DELAY);
            xSemaphoreGive(s_spi_mutex);
        }
#endif
        return;
    }

    if (!s_refresh_paused) {
        return;
    }
    s_refresh_paused = false;
    display_set_static(s_refresh_was_static);
}

