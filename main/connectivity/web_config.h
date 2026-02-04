#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t web_config_start(void);
esp_err_t web_config_stop(void);

#ifdef __cplusplus
}
#endif

