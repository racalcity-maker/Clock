#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "app_control.h"
#include "config_store.h"
#include "encoder.h"

typedef enum {
    UI_MENU_ACTION_NONE,
    UI_MENU_ACTION_HANDLED,
    UI_MENU_ACTION_ENTER_TIME_SETTING,
    UI_MENU_ACTION_SET_MODE
} ui_menu_action_t;

void ui_menu_init(app_config_t *cfg, uint8_t *display_brightness);
bool ui_menu_is_active(void);
void ui_menu_enter(void);
void ui_menu_exit(void);
void ui_menu_render(void);
ui_menu_action_t ui_menu_handle_encoder(encoder_event_t event, app_ui_mode_t *out_mode);
