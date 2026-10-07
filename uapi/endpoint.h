#pragma once

#include <stddef.h>
#include <stdint.h>
#include <uapi/capability.h>
#include <uapi/ipc.h>
#include <uapi/types.h>

#define ENDPOINT_VERSION 4U
#define ENDPOINT_MAX_ACTIVATIONS 8U
#define ENDPOINT_MAX_CALLS 32U
#define ENDPOINT_MAX_DEPTH 16U
#define ENDPOINT_MAX_CODE_PAGES 16U
#define ENDPOINT_MAX_STACK_PAGES 8U
#define ENDPOINT_FLAGS_NONE 0U
#define ENDPOINT_MAX_CAPS 8U

// Immutable service registration. Code and each stack must already be mapped
// in the service VSpace with the stated permissions. The stack object/range is
// split into activation_count equal stack_pages regions.
struct EpDesc {
    uint32_t version;
    uint32_t flags;
    word_t entry;
    cap_t code_memory;
    word_t code_page;
    word_t code_address;
    word_t code_pages;
    cap_t stack_memory;
    word_t stack_page;
    word_t stack_address;
    word_t stack_pages;
    word_t stack_stride;
    struct IpcBinding ipc;
    word_t ipc_stride;
    word_t activation_count;
    word_t queue_capacity;
    word_t max_depth;
    word_t budget_floor_ns;
    word_t urgency_ceiling;
};

#ifdef __cplusplus
static_assert(sizeof(EpDesc) == 168);
static_assert(offsetof(EpDesc, ipc) == 88);
#endif
