#pragma once

#include_next <time.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { CLOCK_MONOTONIC = 1 };

// Zero on success; -1 with errno on failure.
int32_t clock_gettime(int32_t clock, struct timespec* time);
int32_t nanosleep(const struct timespec* delay, struct timespec* remaining);

#ifdef __cplusplus
}
#endif
