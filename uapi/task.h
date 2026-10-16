#pragma once

#include <uapi/abi.h>
#include <uapi/ipc.h>

#define THREAD_START_VERSION 2U

// Immutable constructor snapshot read from an authorized MemoryObject. The
// kernel never follows a transient user pointer while building a Thread.
struct ThreadInit {
    uint32_t version;
    uint32_t flags;
    word_t entry;
    word_t stack;
    word_t arguments[6];
    struct IpcBinding ipc;
};

#ifdef __cplusplus
static_assert(sizeof(ThreadInit) == 104);
#endif

#define THREAD_ENABLED  (1U << 0)
#define THREAD_APPLIED  (1U << 1)
#define THREAD_RUNNING  (1U << 2)
#define THREAD_WAITING  (1U << 3)
#define THREAD_BOUND    (1U << 4)
#define THREAD_STOPPING (1U << 5)
#define THREAD_STOPPED  (1U << 6)

// All time fields use the monotonic clock's ticks, including SC cap/debt.
struct ThreadInfo {
    uint32_t version, size;
    uint64_t requested, applied, runtime, association;
    uint32_t flags, cpu;
};
struct ScInfo {
    uint32_t version, size;
    uint64_t generation, applied, cpus, runtime;
    uint64_t budget, period, available, debt;
    uint32_t weight, bound;
};
