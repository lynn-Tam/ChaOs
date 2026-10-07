#pragma once

#include <stddef.h>
#include <stdint.h>
#include <uapi/capability.h>
#include <uapi/types.h>

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
