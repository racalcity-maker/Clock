#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void audio_tones_play_alarm(uint8_t volume);
void audio_tones_play_system(uint8_t tone, uint8_t volume);

enum {
    AUDIO_SYS_TONE_BT_CONNECT = 1,
    AUDIO_SYS_TONE_BT_DISCONNECT = 2
};

#ifdef __cplusplus
}
#endif
