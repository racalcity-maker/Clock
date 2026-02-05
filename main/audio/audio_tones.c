#include "audio_tones.h"

#include "audio_pcm5102.h"

static uint8_t system_tone_volume(uint8_t volume)
{
    // Keep system tones softer than music/alarm to avoid sharp square-wave clicks.
    uint16_t scaled = ((uint16_t)volume * 60U + 50U) / 100U;
    if (scaled < 64U) {
        scaled = 64U;
    }
    if (scaled > 180U) {
        scaled = 180U;
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
    static const uint16_t chord_step_ms = 300;
    static const uint16_t chord_attack_ms = 8;
    static const uint16_t chord_decay_ms = 90;
    static const uint16_t chord_release_ms = 62;
    static const uint16_t chord_sustain_q15 = 24000;

    static const audio_chord_step_t bt_connect[] = {
        { {371, 467, 554}, { -4, 0, 4 }, chord_step_ms, chord_attack_ms, chord_decay_ms, chord_sustain_q15, chord_release_ms },
        { {277, 349, 416}, { -4, 0, 4 }, chord_step_ms, chord_attack_ms, chord_decay_ms, chord_sustain_q15, chord_release_ms }
    };
    static const audio_chord_step_t bt_disconnect[] = {
        { {311, 370, 467}, { -4, 0, 4 }, chord_step_ms, chord_attack_ms, chord_decay_ms, chord_sustain_q15, chord_release_ms },
        { {233, 277, 349}, { -4, 0, 4 }, chord_step_ms, chord_attack_ms, chord_decay_ms, chord_sustain_q15, chord_release_ms }
    };
    uint8_t sys_volume = system_tone_volume(volume);

    switch (tone) {
        case AUDIO_SYS_TONE_BT_CONNECT:
            audio_play_chord_sequence(bt_connect, sizeof(bt_connect) / sizeof(bt_connect[0]), sys_volume);
            break;
        case AUDIO_SYS_TONE_BT_DISCONNECT:
            audio_play_chord_sequence(bt_disconnect, sizeof(bt_disconnect) / sizeof(bt_disconnect[0]), sys_volume);
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
