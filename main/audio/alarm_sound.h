#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void alarm_sound_init(void);
void alarm_sound_stop(void);
bool alarm_sound_is_playing(void);
uint8_t alarm_sound_get_file_count(void);
bool alarm_sound_play_index(uint8_t index, uint8_t volume_steps, uint32_t preview_ms);
bool alarm_sound_play_builtin(uint8_t tone, uint8_t volume_steps, uint32_t duration_ms);

#ifdef __cplusplus
}
#endif
