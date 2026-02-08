#include "radio_rda5807.h"

#include "app_control.h"
#include "board_pins.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "radio_rda";

#define RADIO_I2C_PORT I2C_NUM_0
#define RADIO_I2C_ADDR_RDA5807 0x10
#define RADIO_I2C_FREQ_HZ 50000
#define RADIO_I2C_TIMEOUT_MS 50
#define RADIO_INIT_SEEK_DELAY_MS 200

#define RADIO_FREQ_MIN_KHZ 87000U
#define RADIO_FREQ_MAX_KHZ 108000U
#define RADIO_FREQ_STEP_KHZ 100U
#define RADIO_DEFAULT_FREQ_KHZ 101700U
#define RADIO_TUNE_DELAY_MS 60
#define RADIO_SEEK_POLL_MS 50
#define RADIO_SEEK_TIMEOUT_MS 2500
#define RADIO_RSSI_MIN 36U

enum {
    REG02_HI = 0,
    REG02_LO,
    REG03_HI,
    REG03_LO,
    REG04_HI,
    REG04_LO,
    REG05_HI,
    REG05_LO,
    REG06_HI,
    REG06_LO,
    REG07_HI,
    REG07_LO,
    REGW_COUNT
};

#define REG02_DHIZ_MASK  (1U << 7)
#define REG02_DMUTE_MASK (1U << 6)
#define REG02_MONO_MASK (1U << 5)
#define REG02_BASS_MASK (1U << 4)
#define REG02_ENABLE_MASK (1U << 0)
#define REG02_SEEK_MASK (1U << 0)   // high byte bit0 (REG02[8])
#define REG02_SEEKUP_MASK (1U << 1) // high byte bit1 (REG02[9])
#define REG02_SKMODE_MASK (1U << 7) // low byte bit7 (REG02[7])

#define REG03_TUNE_MASK 0x10
#define REG03_CHAN_LOW_MASK 0xC0
#define REG03_BAND_SPACE_MASK 0x0F
#define REG03_BAND_87_108 0x00
#define REG03_SPACE_100K 0x00

#define STATUS0_STC_MASK 0x40
#define STATUS0_SF_MASK 0x20

// Minimal init: program 0x02-0x05 (8 bytes). RDA5807M auto-increments from 0x02.
#define REG02_INIT_HI (REG02_DHIZ_MASK | REG02_DMUTE_MASK)
#define REG02_INIT_LO (REG02_ENABLE_MASK)
#define REG04_INIT_HI 0x08 // DE=1 -> 50us
#define REG05_INIT_HI 0x80 // INT_MODE=1
#define REG05_INIT_LO 0x0F // max volume (log scale)

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;
static SemaphoreHandle_t s_lock = NULL;
static uint8_t s_regw[REGW_COUNT];
static uint32_t s_freq_khz = RADIO_DEFAULT_FREQ_KHZ;
static uint8_t s_volume_steps = 12;
static bool s_ready = false;
static bool s_muted = false;
static bool s_seek_in_progress = false;

static void radio_lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void radio_unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static esp_err_t radio_write_regs(size_t len)
{
    if (!s_dev || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit(s_dev, s_regw, len, RADIO_I2C_TIMEOUT_MS);
}

static esp_err_t radio_read_status(uint8_t *buf, size_t len)
{
    if (!s_dev || !buf || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_receive(s_dev, buf, len, RADIO_I2C_TIMEOUT_MS);
}

static void radio_apply_defaults(void)
{
    memset(s_regw, 0, sizeof(s_regw));
    s_regw[REG02_HI] = REG02_INIT_HI;
    s_regw[REG02_LO] = REG02_INIT_LO;
    s_regw[REG03_HI] = 0x00;
    s_regw[REG03_LO] = (uint8_t)(REG03_BAND_87_108 | REG03_SPACE_100K);
    s_regw[REG04_HI] = REG04_INIT_HI;
    s_regw[REG04_LO] = 0x00;
    s_regw[REG05_HI] = REG05_INIT_HI;
    s_regw[REG05_LO] = REG05_INIT_LO;
}

static uint16_t radio_freq_to_chan(uint32_t freq_khz)
{
    if (freq_khz < RADIO_FREQ_MIN_KHZ) {
        freq_khz = RADIO_FREQ_MIN_KHZ;
    } else if (freq_khz > RADIO_FREQ_MAX_KHZ) {
        freq_khz = RADIO_FREQ_MAX_KHZ;
    }
    return (uint16_t)((freq_khz - 87000U) / RADIO_FREQ_STEP_KHZ);
}

static void radio_log_seek_state(const char *state)
{
    if (!state) {
        return;
    }
    ESP_LOGI(TAG, "seek %s (freq=%ukHz)", state, (unsigned)s_freq_khz);
}

static uint8_t radio_volume_from_steps(uint8_t steps)
{
    if (steps > APP_VOLUME_MAX) {
        steps = APP_VOLUME_MAX;
    }
    uint32_t scaled = ((uint32_t)steps * 15U + (APP_VOLUME_MAX / 2U)) / APP_VOLUME_MAX;
    if (scaled > 15U) {
        scaled = 15U;
    }
    return (uint8_t)scaled;
}

esp_err_t radio_rda5807_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    if (PIN_RADIO_SDA == GPIO_NUM_NC || PIN_RADIO_SCL == GPIO_NUM_NC) {
        ESP_LOGW(TAG, "radio i2c pins not set");
        return ESP_ERR_INVALID_STATE;
    }

    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = RADIO_I2C_PORT,
        .scl_io_num = PIN_RADIO_SCL,
        .sda_io_num = PIN_RADIO_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2c bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = RADIO_I2C_ADDR_RDA5807,
        .scl_speed_hz = RADIO_I2C_FREQ_HZ
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK || !s_dev) {
        ESP_LOGW(TAG, "i2c add device failed: %s", esp_err_to_name(err));
        if (s_bus) {
            i2c_del_master_bus(s_bus);
            s_bus = NULL;
        }
        return err != ESP_OK ? err : ESP_ERR_INVALID_STATE;
    }

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }

    s_regw[REG02_HI] = 0x00;
    s_regw[REG02_LO] = 0x02; // soft reset
    radio_lock();
    (void)radio_write_regs(2);
    radio_unlock();
    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t chip_id[10] = {0};
    radio_lock();
    if (radio_read_status(chip_id, sizeof(chip_id)) == ESP_OK) {
        uint16_t id = ((uint16_t)chip_id[8] << 8) | chip_id[9];
        ESP_LOGI(TAG, "chip id: 0x%04X", id);
    }
    radio_unlock();
    vTaskDelay(pdMS_TO_TICKS(50));

    radio_apply_defaults();
    radio_lock();
    err = radio_write_regs(8);
    radio_unlock();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "radio init write failed: %s", esp_err_to_name(err));
        if (s_dev) {
            i2c_master_bus_rm_device(s_dev);
            s_dev = NULL;
        }
        if (s_bus) {
            i2c_del_master_bus(s_bus);
            s_bus = NULL;
        }
        return err;
    }

    s_muted = false;
    s_ready = true;

    ESP_LOGI(TAG, "radio ready (RDA addr=0x%02X)", RADIO_I2C_ADDR_RDA5807);
    return ESP_OK;
}

void radio_rda5807_deinit(void)
{
    if (!s_ready) {
        return;
    }
    radio_rda5807_set_muted(true);
    radio_rda5807_set_enabled(false);
    vTaskDelay(pdMS_TO_TICKS(50));

    if (s_dev) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    if (s_bus) {
        i2c_del_master_bus(s_bus);
    }
    s_bus = NULL;
    s_ready = false;
}

bool radio_rda5807_is_ready(void)
{
    return s_ready;
}

void radio_rda5807_set_enabled(bool enabled)
{
    if (!s_ready) {
        return;
    }
    radio_lock();
    if (enabled) {
        s_regw[REG02_LO] |= REG02_ENABLE_MASK;
    } else {
        s_regw[REG02_LO] &= ~REG02_ENABLE_MASK;
    }
    (void)radio_write_regs(2);
    radio_unlock();
}

void radio_rda5807_set_muted(bool muted)
{
    s_muted = muted;
    if (!s_ready) {
        return;
    }
    radio_lock();
    if (muted) {
        s_regw[REG02_HI] &= (uint8_t)~REG02_DMUTE_MASK;
    } else {
        s_regw[REG02_HI] |= REG02_DMUTE_MASK;
    }
    (void)radio_write_regs(2);
    radio_unlock();
}

void radio_rda5807_set_volume_steps(uint8_t steps)
{
    s_volume_steps = steps;
    if (!s_ready) {
        return;
    }
    uint8_t vol = radio_volume_from_steps(steps);
    radio_lock();
    s_regw[REG05_LO] = (uint8_t)((s_regw[REG05_LO] & 0xF0U) | (vol & 0x0FU));
    s_regw[REG03_LO] &= (uint8_t)~REG03_TUNE_MASK;
    (void)radio_write_regs(8);
    radio_unlock();
}

uint8_t radio_rda5807_get_volume_steps(void)
{
    return s_volume_steps;
}

void radio_rda5807_tune_khz(uint32_t freq_khz)
{
    if (freq_khz < RADIO_FREQ_MIN_KHZ) {
        freq_khz = RADIO_FREQ_MIN_KHZ;
    } else if (freq_khz > RADIO_FREQ_MAX_KHZ) {
        freq_khz = RADIO_FREQ_MAX_KHZ;
    }
    uint32_t aligned = (freq_khz / RADIO_FREQ_STEP_KHZ) * RADIO_FREQ_STEP_KHZ;
    s_freq_khz = aligned;

    if (!s_ready) {
        return;
    }

    uint16_t chan = radio_freq_to_chan(aligned);

    radio_lock();
    if (!s_muted) {
        s_regw[REG02_HI] |= REG02_DMUTE_MASK;
    }
    s_regw[REG03_HI] = (uint8_t)(chan >> 2);
    s_regw[REG03_LO] = (uint8_t)((s_regw[REG03_LO] & REG03_CHAN_LOW_MASK) |
                                 (REG03_BAND_87_108 | REG03_SPACE_100K));
    s_regw[REG03_LO] = (uint8_t)((s_regw[REG03_LO] & ~REG03_CHAN_LOW_MASK) |
                                 ((chan & 0x03U) << 6));
    s_regw[REG03_LO] |= REG03_TUNE_MASK;
    (void)radio_write_regs(4);
    radio_unlock();

    vTaskDelay(pdMS_TO_TICKS(RADIO_TUNE_DELAY_MS));

    radio_lock();
    s_regw[REG03_LO] &= (uint8_t)~REG03_TUNE_MASK;
    (void)radio_write_regs(4);
    radio_unlock();
}

void radio_rda5807_step(bool up)
{
    uint32_t freq = s_freq_khz ? s_freq_khz : RADIO_DEFAULT_FREQ_KHZ;
    if (up) {
        if (freq + RADIO_FREQ_STEP_KHZ > RADIO_FREQ_MAX_KHZ) {
            freq = RADIO_FREQ_MIN_KHZ;
        } else {
            freq += RADIO_FREQ_STEP_KHZ;
        }
    } else {
        if (freq < RADIO_FREQ_MIN_KHZ + RADIO_FREQ_STEP_KHZ) {
            freq = RADIO_FREQ_MAX_KHZ;
        } else {
            freq -= RADIO_FREQ_STEP_KHZ;
        }
    }
    radio_rda5807_tune_khz(freq);
}

uint32_t radio_rda5807_get_frequency_khz(void)
{
    return s_freq_khz;
}

bool radio_rda5807_autoseek(bool up)
{
    if (!s_ready) {
        return false;
    }
    if (s_seek_in_progress) {
        return false;
    }

    radio_log_seek_state("start");
    s_seek_in_progress = true;

    radio_lock();
    if (up) {
        s_regw[REG02_HI] |= REG02_SEEKUP_MASK;
    } else {
        s_regw[REG02_HI] &= (uint8_t)~REG02_SEEKUP_MASK;
    }
    s_regw[REG02_LO] &= (uint8_t)~REG02_SKMODE_MASK; // wrap
    s_regw[REG02_HI] |= REG02_SEEK_MASK;
    (void)radio_write_regs(2);
    radio_unlock();

    int64_t start_us = esp_timer_get_time();
    uint8_t status[4] = {0};
    bool ok = false;
    while ((esp_timer_get_time() - start_us) < (int64_t)RADIO_SEEK_TIMEOUT_MS * 1000) {
        vTaskDelay(pdMS_TO_TICKS(RADIO_SEEK_POLL_MS));
        radio_lock();
        esp_err_t err = radio_read_status(status, sizeof(status));
        radio_unlock();
        if (err != ESP_OK) {
            continue;
        }
        if ((status[0] & STATUS0_STC_MASK) == 0) {
            continue;
        }
        bool seek_fail = (status[0] & STATUS0_SF_MASK) != 0;
        uint16_t chan = (uint16_t)(((status[0] & 0x03U) << 8) | status[1]);
        s_freq_khz = RADIO_FREQ_MIN_KHZ + ((uint32_t)chan * RADIO_FREQ_STEP_KHZ);
        ok = !seek_fail;
        break;
    }

    radio_lock();
    s_regw[REG02_HI] &= (uint8_t)~REG02_SEEK_MASK;
    (void)radio_write_regs(2);
    radio_unlock();

    s_seek_in_progress = false;
    if (ok) {
        radio_log_seek_state("done");
    } else {
        radio_log_seek_state("timeout");
    }
    return ok;
}

uint32_t radio_rda5807_get_init_seek_delay_ms(void)
{
    return RADIO_INIT_SEEK_DELAY_MS;
}
