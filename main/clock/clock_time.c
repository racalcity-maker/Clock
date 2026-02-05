#include "clock_time.h"

#include "string.h"
#include "time.h"
#include <stdlib.h>

static bool s_time_valid = false;

void clock_time_init(const char *tz)
{
    clock_time_set_timezone(tz);
    s_time_valid = false;
}

void clock_time_set_timezone(const char *tz)
{
    const char *safe_tz = (tz && strlen(tz) > 0) ? tz : "UTC0";
    setenv("TZ", safe_tz, 1);
    tzset();
}

void clock_time_get(struct tm *out)
{
    if (!out) {
        return;
    }
    time_t now = 0;
    time(&now);
    localtime_r(&now, out);
}

void clock_time_mark_valid(void)
{
    s_time_valid = true;
}

bool clock_time_is_valid(void)
{
    return s_time_valid;
}

