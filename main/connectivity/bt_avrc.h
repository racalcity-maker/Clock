#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BT_AVRC_CMD_PLAY,
    BT_AVRC_CMD_PAUSE,
    BT_AVRC_CMD_NEXT,
    BT_AVRC_CMD_PREV,
    BT_AVRC_CMD_STOP
} bt_avrc_cmd_t;

typedef void (*bt_avrc_volume_cb_t)(uint8_t volume);

esp_err_t bt_avrc_init(void);
void bt_avrc_deinit(void);
esp_err_t bt_avrc_send_command(bt_avrc_cmd_t cmd);
bool bt_avrc_is_connected(void);
void bt_avrc_register_volume_cb(bt_avrc_volume_cb_t cb);
void bt_avrc_notify_volume(uint8_t volume);

#ifdef __cplusplus
}
#endif
