#pragma once

#include_next <time.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 4
#endif
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 1
#endif

// Zero on success; -1 with errno on failure.
int clock_gettime(clockid_t clock, struct timespec* time);
int32_t nanosleep(const struct timespec* delay, struct timespec* remaining);

#ifdef __cplusplus
}
#endif
