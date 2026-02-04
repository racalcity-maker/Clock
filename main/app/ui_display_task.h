#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "config_store.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_display_task_init(app_config_t *cfg, uint8_t *volume_level, bool *soft_off);
void ui_display_task_start(void);
void ui_display_task_pause(void);
void ui_display_task_resume(void);
void ui_display_task_notify_bt_volume(uint8_t volume);
void ui_display_task_show_volume(uint8_t volume);
void ui_display_task_clear_overlay(void);
void ui_display_task_show_track_overlay(uint16_t track_index, uint16_t track_count);
void ui_display_task_mark_volume_dirty(void);
void ui_display_task_set_overlays_enabled(bool enabled);

#ifdef __cplusplus
}
#endif
