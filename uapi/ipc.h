#pragma once

#include <stddef.h>
#include <stdint.h>
#include <uapi/cap.h>
#include <uapi/abi.h>

#define IPC_MAX_CAPS 8U
#define IPC_BUFFER_MAX_PAGES 2U

#define CAP_COPY     0U
#define CAP_MOVE     1U
#define CAP_DELEGATE 2U

// Immutable registration for one execution-local IPC buffer. A zero page
// count disables buffer-based operations.
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

#ifdef __cplusplus
static_assert(sizeof(IpcBinding) == 32);
static_assert(sizeof(CapXfer) == 24);
#endif

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
