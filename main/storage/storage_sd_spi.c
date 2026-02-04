#include "storage_sd_spi.h"

#include "board_pins.h"
#include "driver/spi_common.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "sdmmc_cmd.h"

static const char *TAG = "storage_sd";
static const char *MOUNT_POINT = "/sdcard";
static sdmmc_card_t *s_card = NULL;
static bool s_spi_owns_bus = false;
static bool s_spi_bus_ready = false;

esp_err_t storage_sd_init(void)
{
    if (s_card) {
        return ESP_OK;
    }
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_SD_MOSI,
        .miso_io_num = PIN_SD_MISO,
        .sclk_io_num = PIN_SD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 16 * 1024
    };

    esp_err_t err = ESP_OK;
    if (!s_spi_bus_ready) {
        err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "spi bus init failed: %s", esp_err_to_name(err));
            return err;
        }
        if (err == ESP_OK) {
            s_spi_owns_bus = true;
        }
        s_spi_bus_ready = true;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_SD_CS;
    slot_config.host_id = SPI2_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024
    };

    err = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sd mount failed: %s", esp_err_to_name(err));
        if (s_spi_owns_bus) {
            esp_err_t free_err = spi_bus_free(SPI2_HOST);
            if (free_err != ESP_OK) {
                ESP_LOGW(TAG, "spi bus free failed: %s", esp_err_to_name(free_err));
            } else {
                s_spi_owns_bus = false;
            }
        }
        s_spi_bus_ready = s_spi_owns_bus;
        return err;
    }
    return ESP_OK;
}

void storage_sd_unmount(void)
{
    if (!s_card) {
        return;
    }
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
    s_card = NULL;

    if (s_spi_owns_bus) {
        s_spi_owns_bus = false;
    }
}

bool storage_sd_is_mounted(void)
{
    return s_card != NULL;
}

esp_err_t storage_sd_get_space_mb(uint32_t *free_mb, uint32_t *total_mb)
{
    if (!s_card) {
        return ESP_ERR_INVALID_STATE;
    }

    FATFS *fs = NULL;
    DWORD free_clusters = 0;
    FRESULT res = f_getfree(MOUNT_POINT, &free_clusters, &fs);
    if (res != FR_OK || !fs) {
        return ESP_FAIL;
    }

    uint64_t total_sectors = (uint64_t)(fs->n_fatent - 2U) * fs->csize;
    uint64_t free_sectors = (uint64_t)free_clusters * fs->csize;
    uint64_t total_bytes = total_sectors * 512ULL;
    uint64_t free_bytes = free_sectors * 512ULL;

    if (total_mb) {
        *total_mb = (uint32_t)(total_bytes / (1024ULL * 1024ULL));
    }
    if (free_mb) {
        *free_mb = (uint32_t)(free_bytes / (1024ULL * 1024ULL));
    }
    return ESP_OK;
}

