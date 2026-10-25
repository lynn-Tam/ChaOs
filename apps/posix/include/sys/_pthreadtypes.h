#pragma once
// Newlib's sys/types.h includes this target ABI before pthread.h itself.
#include <stddef.h>
#include <stdint.h>
#include <sync.hpp>
typedef uint64_t pthread_t;
typedef unsigned pthread_key_t;
typedef _mutex pthread_mutex_t;
typedef _once pthread_once_t;
typedef struct { size_t stack_size, guard_size; int detached; } pthread_attr_t;
typedef struct { int type; } pthread_mutexattr_t;
typedef struct { _cond wait; int clock; } pthread_cond_t;
typedef struct { int clock; } pthread_condattr_t;
