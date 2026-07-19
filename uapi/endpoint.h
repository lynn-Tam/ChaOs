#pragma once

#include <stdint.h>
#include <uapi/capability.h>
#include <uapi/ipc.h>
#include <uapi/types.h>

#define MYOS_ENDPOINT_VERSION 4U
#define MYOS_ENDPOINT_MAX_ACTIVATIONS 8U
#define MYOS_ENDPOINT_MAX_CALLS 32U
#define MYOS_ENDPOINT_MAX_DEPTH 16U
#define MYOS_ENDPOINT_MAX_CODE_PAGES 16U
#define MYOS_ENDPOINT_MAX_STACK_PAGES 8U
#define MYOS_ENDPOINT_FLAGS_NONE 0U
#define MYOS_ENDPOINT_MAX_CAPS 8U

// Immutable service registration. Code and each stack must already be mapped
// in the service VSpace with the stated permissions. The stack object/range is
// split into activation_count equal stack_pages regions.
struct myos_endpoint_desc {
    uint32_t version;
    uint32_t flags;
    myos_word_t entry;
    myos_cap_t code_memory;
    myos_word_t code_page;
    myos_word_t code_address;
    myos_word_t code_pages;
    myos_cap_t stack_memory;
    myos_word_t stack_page;
    myos_word_t stack_address;
    myos_word_t stack_pages;
    myos_word_t stack_stride;
    struct myos_ipc_binding ipc;
    myos_word_t ipc_stride;
    myos_word_t activation_count;
    myos_word_t queue_capacity;
    myos_word_t max_depth;
    myos_word_t budget_floor_ns;
    myos_word_t urgency_ceiling;
};
