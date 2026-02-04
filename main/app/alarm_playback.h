#pragma once

#include "config_store.h"

#ifdef __cplusplus
extern "C" {
#endif

void alarm_playback_init(void);
void alarm_playback_start(const app_config_t *cfg);
void alarm_playback_stop(void);

#ifdef __cplusplus
}
#endif
