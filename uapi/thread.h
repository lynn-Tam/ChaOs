#pragma once

#include <stdint.h>
#include <uapi/ipc.h>
#include <uapi/types.h>

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
