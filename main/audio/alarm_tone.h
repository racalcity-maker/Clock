#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool alarm_tone_is_playing(void);
bool alarm_tone_play(uint8_t tone, uint8_t volume_steps, uint32_t duration_ms);
void alarm_tone_stop(void);

#ifdef __cplusplus
}
#endif
