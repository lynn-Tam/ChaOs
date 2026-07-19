#pragma once

#include <stdint.h>
#include <uapi/capability.h>
#include <uapi/types.h>

#define MYOS_IPC_MAX_CAPS 8U
#define MYOS_IPC_BUFFER_MAX_PAGES 2U
#define MYOS_IPC_CAPS_VERSION 1U
#define MYOS_IPC_CAPS_FLAGS_NONE 0U

#define MYOS_CAP_COPY     0U
#define MYOS_CAP_MOVE     1U
#define MYOS_CAP_DELEGATE 2U

// Immutable registration for one execution-local IPC buffer. A zero page
// count means that the execution supports only the register message subset.
struct myos_ipc_binding {
    myos_cap_t memory;
    myos_word_t page;
    myos_word_t address;
    myos_word_t pages;
};

// A bounded capability transfer request. Rights are an attenuation for copy
// and delegate; move requires zero and preserves the complete source view.
struct myos_cap_transfer {
    myos_cap_t source;
    uint64_t rights;
    uint32_t operation;
    uint32_t flags;
};

// One execution-local capability exchange area. send[] is snapshotted when
// an IPC operation is admitted. received[] is a projection of destination
// reservations and becomes authoritative only when received_count is
// published after the all-or-nothing CSpace commit.
struct myos_ipc_caps {
    uint32_t version;
    uint32_t flags;
    uint32_t send_count;
    uint32_t receive_limit;
    struct myos_cap_transfer send[MYOS_IPC_MAX_CAPS];
    myos_cap_t received[MYOS_IPC_MAX_CAPS];
    uint32_t received_count;
    uint32_t reserved;
};
