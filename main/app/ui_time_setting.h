#pragma once

#include <stdbool.h>

void ui_time_setting_enter(void);
bool ui_time_setting_is_active(void);
void ui_time_setting_reset(void);
bool ui_time_setting_handle_short_press(void);
bool ui_time_setting_handle_knob(int delta);
bool ui_time_setting_should_exit(void);
void ui_time_setting_render(void);
