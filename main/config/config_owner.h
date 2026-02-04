#pragma once

#include "config_store.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void config_owner_init(app_config_t *cfg);
void config_owner_start(void);
bool config_owner_request_update(const app_config_t *cfg);

#ifdef __cplusplus
}
#endif
