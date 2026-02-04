#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t storage_sd_init(void);
void storage_sd_unmount(void);
bool storage_sd_is_mounted(void);
esp_err_t storage_sd_get_space_mb(uint32_t *free_mb, uint32_t *total_mb);

#ifdef __cplusplus
}
#endif

