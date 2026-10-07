#pragma once

#include <stddef.h>
#include <stdint.h>
#include <uapi/cap.h>
#include <uapi/abi.h>

#define IPC_MAX_CAPS 8U
#define IPC_BUFFER_MAX_PAGES 2U
#define IPC_CAPS_VERSION 1U
#define IPC_CAPS_FLAGS_NONE 0U

#define CAP_COPY     0U
#define CAP_MOVE     1U
#define CAP_DELEGATE 2U

// Immutable registration for one execution-local IPC buffer. A zero page
// count means that the execution supports only the register message subset.
struct IpcBinding {
    cap_t memory;
    word_t page;
    word_t address;
    word_t pages;
};

// A bounded capability transfer request. Rights are an attenuation for copy
// and delegate; move requires zero and preserves the complete source view.
struct CapXfer {
    cap_t source;
    uint64_t rights;
    uint32_t operation;
    uint32_t flags;
};

// One execution-local capability exchange area. send[] is snapshotted when
// an IPC operation is admitted. received[] is a projection of destination
// reservations and becomes authoritative only when received_count is
// published after the all-or-nothing CSpace commit.
struct IpcCaps {
    uint32_t version;
    uint32_t flags;
    uint32_t send_count;
    uint32_t receive_limit;
    struct CapXfer send[IPC_MAX_CAPS];
    cap_t received[IPC_MAX_CAPS];
    uint32_t received_count;
    uint32_t reserved;
};

#ifdef __cplusplus
static_assert(sizeof(IpcBinding) == 32);
static_assert(sizeof(CapXfer) == 24);
static_assert(sizeof(IpcCaps) == 280);
static_assert(offsetof(IpcCaps, send) == 16);
static_assert(offsetof(IpcCaps, received_count) == 272);
#endif

#include <stddef.h>
#include <stdint.h>

#define CHANNEL_VERSION 1U
#define CHANNEL_FLAGS_NONE 0U
// Queue storage is prepaid at creation; depth is a 32-bit configuration value.
#define CHANNEL_MAX_QUEUE UINT32_MAX
#define CHANNEL_MAX_WORDS 16U
#define CHANNEL_MAX_CAPS IPC_MAX_CAPS
// The relation handle dedicates eight bits to its slot.
#define CHANNEL_MAX_RELATIONS 256U

// ARM returns the current sequence for the next recheck/arm cycle.
// Readable/Writable also become ready on close: the corresponding operation
// can return a terminal status. Notifications are hints; retry the operation
// to distinguish queued data, available space, and closure.
#define CHANNEL_READABLE 0U
#define CHANNEL_WRITABLE 1U
#define CHANNEL_PEER_CLOSED 2U

struct ChanMsg {
    uint32_t version;
    uint32_t flags;
    uint32_t word_count;
    uint32_t cap_count;
    uint32_t receive_limit;
    uint32_t reserved;
    word_t transaction;
    word_t tag;
    word_t words[CHANNEL_MAX_WORDS];
    struct CapXfer caps[CHANNEL_MAX_CAPS];
    cap_t received[CHANNEL_MAX_CAPS];
    uint32_t received_count;
    uint32_t received_reserved;
    word_t sender_badge;
    word_t sequence;
};

#ifdef __cplusplus
static_assert(sizeof(ChanMsg) == 448);
static_assert(offsetof(ChanMsg, caps) == 168);
static_assert(offsetof(ChanMsg, received_count) == 424);
#endif

#include <stddef.h>
#include <stdint.h>

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
