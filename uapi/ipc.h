#pragma once

#include <stddef.h>
#include <stdint.h>
#include <uapi/cap.h>
#include <uapi/abi.h>

#define IPC_MAX_CAPS 8U
#define IPC_BUFFER_MAX_PAGES 2U

#define SOURCE_STATE 1U
#define SOURCE_READ 2U
#define SOURCE_WRITE 3U
#define SOURCE_EXIT 4U
#define SOURCE_GRANT 5U
#define SOURCE_READY 1U
#define SOURCE_TERMINAL 2U
#define WATCH_CHANGED 1U
#define PORT_READABLE 1U
#define PORT_OVERFLOW 2U
#define PORT_EVENT_CLOSED 1U
#define PORT_EVENT_PENDING 2U
#define PORT_RECORDS 8U
struct PortEvent {
    uint32_t version, size;
    uint64_t source_id, key, watch_gen, sequence, condition;
    uint32_t facet, flags;
};
struct PortRecords {
    uint32_t version, size;
    uint64_t epoch, next;
    uint32_t count, more;
    struct PortEvent events[PORT_RECORDS];
};

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
#define CHANNEL_FLAG_CALL 1U /* receive-only: cap[0] is the kernel Reply facet */
#define CHANNEL_DISCARD 1U
// CALL creates a kernel-only, movable, single-use reply facet as received[0].
// CANCEL races reply at the reserved result cell; DISCARD releases that result.
// Call tokens select generations within an authorized transport, never authority.
// Request and reply may share a Channel, but must use opposite directions.
// Receive NO_MEMORY preserves the head message and returns its cap count in value.
// Try-send capacity prechecks are conservative hints: WOULD_BLOCK accepts no
// request. Wait/arm checks Source facts again before parking.
// Queue storage is prepaid at creation; depth is a 32-bit configuration value.
#define CHANNEL_MAX_QUEUE UINT32_MAX
#define CHANNEL_MAX_WORDS 16U
#define CHANNEL_MAX_CAPS IPC_MAX_CAPS
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
