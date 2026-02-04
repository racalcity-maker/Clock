#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void audio_eq_init(uint32_t sample_rate);
void audio_eq_set_sample_rate(uint32_t sample_rate);
void audio_eq_set_steps(uint8_t low_step, uint8_t high_step);
bool audio_eq_is_flat(void);
void audio_eq_process(int16_t *samples, size_t frames, int channels);

#ifdef __cplusplus
}
#endif
