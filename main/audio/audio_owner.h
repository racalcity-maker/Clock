#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUDIO_OWNER_NONE = 0,
    AUDIO_OWNER_BT,
    AUDIO_OWNER_PLAYER,
    AUDIO_OWNER_ALARM,
    AUDIO_OWNER_TONE
} audio_owner_t;

bool audio_owner_acquire(audio_owner_t owner, bool force);
void audio_owner_release(audio_owner_t owner);
audio_owner_t audio_owner_get(void);
const char *audio_owner_name(audio_owner_t owner);

#ifdef __cplusplus
}
#endif
