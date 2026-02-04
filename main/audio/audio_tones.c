#include "audio_tones.h"

#include "audio_pcm5102.h"

static uint8_t system_tone_volume(uint8_t volume)
{
    // Keep system tones softer than music/alarm to avoid sharp square-wave clicks.
    uint16_t scaled = ((uint16_t)volume * 35U + 50U) / 100U;
    if (scaled < 28U) {
        scaled = 28U;
    }
    if (scaled > 96U) {
        scaled = 96U;
    }
    return (uint8_t)scaled;
}

void audio_tones_play_alarm(uint8_t volume)
{
    static const audio_tone_step_t alarm_beep[] = {
        {2040, 70}, {0, 60},
        {2040, 70}, {0, 60},
        {2040, 70}, {0, 60},
        {2040, 70}, {0, 300}
    };
    audio_play_tone_sequence_blocking(alarm_beep, sizeof(alarm_beep) / sizeof(alarm_beep[0]), volume);
}

void audio_tones_play_system(uint8_t tone, uint8_t volume)
{
    static const audio_pluck_step_t bt_connect[] = {
        {440, 85, 32580}, {0, 20, 32580},
        {660, 120, 32560}
    };
    static const audio_pluck_step_t bt_disconnect[] = {
        {660, 90, 32540}, {0, 20, 32540},
        {392, 130, 32520}
    };
    uint8_t sys_volume = system_tone_volume(volume);

    switch (tone) {
        case AUDIO_SYS_TONE_BT_CONNECT:
            audio_play_pluck_sequence(bt_connect, sizeof(bt_connect) / sizeof(bt_connect[0]), sys_volume);
            break;
        case AUDIO_SYS_TONE_BT_DISCONNECT:
            audio_play_pluck_sequence(bt_disconnect, sizeof(bt_disconnect) / sizeof(bt_disconnect[0]), sys_volume);
            break;
        default: {
            static const audio_tone_step_t placeholder[] = {
                {0, 20}
            };
            audio_play_tone_sequence(placeholder, sizeof(placeholder) / sizeof(placeholder[0]), sys_volume);
            break;
        }
    }
}
