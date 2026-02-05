#pragma once

#include <stdbool.h>
#include "time.h"

#ifdef __cplusplus
extern "C" {
#endif

void clock_time_init(const char *tz);
void clock_time_set_timezone(const char *tz);
void clock_time_get(struct tm *out);
void clock_time_mark_valid(void);
bool clock_time_is_valid(void);

#ifdef __cplusplus
}
#endif

