#pragma once

#include "app_control.h"

#ifdef __cplusplus
extern "C" {
#endif

void alarm_actions_on_trigger(void);
void alarm_actions_on_ack(void);
void alarm_actions_poll(void);

#ifdef __cplusplus
}
#endif
