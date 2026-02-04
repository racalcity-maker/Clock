#include "audio_owner.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "esp_log.h"

static audio_owner_t s_owner = AUDIO_OWNER_NONE;
static portMUX_TYPE s_owner_lock = portMUX_INITIALIZER_UNLOCKED;

static bool audio_owner_can_preempt(audio_owner_t new_owner, audio_owner_t cur_owner)
{
    if (new_owner == AUDIO_OWNER_ALARM) {
        return true;
    }
    if (new_owner == AUDIO_OWNER_TONE && cur_owner == AUDIO_OWNER_BT) {
        return true;
    }
    (void)cur_owner;
    return false;
}

bool audio_owner_acquire(audio_owner_t owner, bool force)
{
    if (owner == AUDIO_OWNER_NONE) {
        return false;
    }

    bool ok = false;
    portENTER_CRITICAL(&s_owner_lock);
    if (s_owner == AUDIO_OWNER_NONE || s_owner == owner) {
        s_owner = owner;
        ok = true;
    } else if (force && audio_owner_can_preempt(owner, s_owner)) {
        s_owner = owner;
        ok = true;
    }
    portEXIT_CRITICAL(&s_owner_lock);
    if (!ok) {
        ESP_LOGW("audio_owner", "acquire fail owner=%s cur=%s force=%d",
                 audio_owner_name(owner),
                 audio_owner_name(audio_owner_get()),
                 force ? 1 : 0);
    }
    return ok;
}

void audio_owner_release(audio_owner_t owner)
{
    portENTER_CRITICAL(&s_owner_lock);
    if (s_owner == owner) {
        s_owner = AUDIO_OWNER_NONE;
    }
    portEXIT_CRITICAL(&s_owner_lock);
}

audio_owner_t audio_owner_get(void)
{
    audio_owner_t cur;
    portENTER_CRITICAL(&s_owner_lock);
    cur = s_owner;
    portEXIT_CRITICAL(&s_owner_lock);
    return cur;
}

const char *audio_owner_name(audio_owner_t owner)
{
    switch (owner) {
        case AUDIO_OWNER_NONE:
            return "none";
        case AUDIO_OWNER_BT:
            return "bt";
        case AUDIO_OWNER_PLAYER:
            return "player";
        case AUDIO_OWNER_ALARM:
            return "alarm";
        case AUDIO_OWNER_TONE:
            return "tone";
        default:
            return "unknown";
    }
}
