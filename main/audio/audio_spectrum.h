#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifndef AUDIO_SPECTRUM_ENABLE
#define AUDIO_SPECTRUM_ENABLE 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

void audio_spectrum_reset(void);
void audio_spectrum_set_sample_rate(uint32_t sample_rate);
void audio_spectrum_feed(const int16_t *samples, size_t sample_count, int channels);
void audio_spectrum_get_levels(uint8_t out_levels[4]);
void audio_spectrum_enable(bool enable);

#ifdef __cplusplus
}
#endif
