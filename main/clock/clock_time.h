#pragma once

#include "time.h"

#ifdef __cplusplus
extern "C" {
#endif

void clock_time_init(const char *tz);
void clock_time_set_timezone(const char *tz);
void clock_time_get(struct tm *out);

#ifdef __cplusplus
}
#endif

