#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define RADIO_FREQ_MIN_KHZ 87000U
#define RADIO_FREQ_MAX_KHZ 108000U
#define RADIO_FREQ_STEP_KHZ 100U

esp_err_t radio_rda5807_init(void);
void radio_rda5807_deinit(void);
bool radio_rda5807_is_ready(void);

void radio_rda5807_set_enabled(bool enabled);
void radio_rda5807_set_muted(bool muted);
void radio_rda5807_set_volume_steps(uint8_t steps);
uint8_t radio_rda5807_get_volume_steps(void);
void radio_rda5807_tune_khz(uint32_t freq_khz);
void radio_rda5807_step(bool up);
uint32_t radio_rda5807_get_frequency_khz(void);
bool radio_rda5807_autoseek(bool up);
uint32_t radio_rda5807_get_init_seek_delay_ms(void);
