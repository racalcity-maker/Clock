#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audio_init(void);
void audio_set_volume(uint8_t volume);
uint8_t audio_get_volume(void);
esp_err_t audio_i2s_set_sample_rate(uint32_t sample_rate);
esp_err_t audio_i2s_write(const void *data, size_t len, size_t *bytes_written, uint32_t timeout_ms);
esp_err_t audio_i2s_reset(void);
void audio_i2s_write_silence(uint32_t duration_ms);
void audio_play_tone(uint16_t freq_hz, uint32_t duration_ms);
typedef struct {
    uint16_t freq_hz;
    uint16_t duration_ms;
} audio_tone_step_t;
typedef struct {
    uint16_t freq_hz;
    uint16_t duration_ms;
    uint16_t damping_q15;
} audio_pluck_step_t;
void audio_play_tone_sequence(const audio_tone_step_t *seq, size_t count, uint8_t volume);
void audio_play_pluck_sequence(const audio_pluck_step_t *seq, size_t count, uint8_t volume);
void audio_play_tone_sequence_blocking(const audio_tone_step_t *seq, size_t count, uint8_t volume);
void audio_play_alarm(void);
void audio_play_alarm_tone(uint8_t tone);
void audio_play_alarm_tone_volume(uint8_t tone, uint8_t volume);
void audio_play_system_tone(uint8_t tone);
void audio_stop(void);

#ifdef __cplusplus
}
#endif

