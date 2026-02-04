#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SEG_DP (1U << 0)
#define SEG_A  (1U << 1)
#define SEG_B  (1U << 2)
#define SEG_C  (1U << 3)
#define SEG_D  (1U << 4)
#define SEG_E  (1U << 5)
#define SEG_F  (1U << 6)
#define SEG_G  (1U << 7)

void display_init(void);
void display_set_segments(const uint8_t segs[4], bool colon);
void display_set_digits(const uint8_t digits[4], bool colon);
void display_set_time(uint8_t hours, uint8_t minutes, bool colon);
void display_set_brightness(uint8_t level);
uint8_t display_get_brightness(void);
void display_set_text(const char text[4], bool colon);
void display_set_static(bool enable);
void display_pause_refresh(bool pause);

#ifdef __cplusplus
}
#endif

