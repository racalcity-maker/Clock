#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PLAYER_REPEAT_ALL,
    PLAYER_REPEAT_ONE,
    PLAYER_REPEAT_SHUFFLE
} audio_repeat_mode_t;

typedef enum {
    PLAYER_STATE_STOPPED,
    PLAYER_STATE_PLAYING,
    PLAYER_STATE_PAUSED
} audio_player_state_t;

esp_err_t audio_player_init(const char *folder);
void audio_player_shutdown(void);
bool audio_player_is_ready(void);
void audio_player_rescan(void);
void audio_player_set_volume(uint8_t volume);
void audio_player_set_repeat_mode(audio_repeat_mode_t mode);
audio_repeat_mode_t audio_player_get_repeat_mode(void);
audio_player_state_t audio_player_get_state(void);
uint16_t audio_player_get_track_index(void);
uint16_t audio_player_get_track_count(void);
void audio_player_get_time_ms(uint32_t *elapsed_ms, uint32_t *total_ms);

void audio_player_play(void);
void audio_player_pause(void);
void audio_player_stop(void);
void audio_player_next(void);
void audio_player_prev(void);

#ifdef __cplusplus
}
#endif

