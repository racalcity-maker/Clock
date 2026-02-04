#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void power_manager_init(void);
void power_manager_set_autonomous(bool enabled);
void power_manager_enter_sleep(uint32_t seconds);
void power_manager_handle_boot(void);
bool power_manager_is_external_power(void);
void power_manager_pause(void);
void power_manager_resume(void);

#ifdef __cplusplus
}
#endif

