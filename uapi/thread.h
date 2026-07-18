#pragma once

#include <stdint.h>
#include <uapi/types.h>

#define MYOS_THREAD_START_VERSION 1U

// Immutable constructor snapshot read from an authorized MemoryObject. The
// kernel never follows a transient user pointer while building a Thread.
struct myos_thread_start {
    uint32_t version;
    uint32_t flags;
    myos_word_t entry;
    myos_word_t stack;
    myos_word_t arguments[6];
};
