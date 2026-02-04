#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void display_ui_init(void);
void display_ui_set_time(uint8_t hours, uint8_t minutes, bool colon);
void display_ui_show_text(const char text[4], uint32_t duration_ms);
void display_ui_show_digits(const uint8_t digits[4], bool colon, uint32_t duration_ms);
void display_ui_show_segments(const uint8_t segs[4], bool colon, uint32_t duration_ms);
void display_ui_render(void);
bool display_ui_overlay_active(void);
bool display_ui_overlay_is_segments(void);

#ifdef __cplusplus
}
#endif

