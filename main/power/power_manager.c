#include "power_manager.h"

// Power manager is intentionally disabled in this hardware profile.
// Keep API as no-op stubs so mode/menu code does not need conditional branches.

void power_manager_init(void)
{
}

void power_manager_set_autonomous(bool enabled)
{
    (void)enabled;
}

void power_manager_enter_sleep(uint32_t seconds)
{
    (void)seconds;
}

void power_manager_handle_boot(void)
{
}

bool power_manager_is_external_power(void)
{
    return true;
}

void power_manager_pause(void)
{
}

void power_manager_resume(void)
{
}

