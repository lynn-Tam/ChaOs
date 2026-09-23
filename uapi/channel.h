#pragma once

#include <stdint.h>
#include <uapi/ipc.h>
#include <uapi/types.h>

#define MYOS_CHANNEL_VERSION 1U
#define MYOS_CHANNEL_FLAGS_NONE 0U
// Queue storage is prepaid at creation; depth is a 32-bit configuration value.
#define MYOS_CHANNEL_MAX_QUEUE UINT32_MAX
#define MYOS_CHANNEL_MAX_WORDS 16U
#define MYOS_CHANNEL_MAX_CAPS MYOS_IPC_MAX_CAPS
// The relation handle dedicates eight bits to its slot.
#define MYOS_CHANNEL_MAX_RELATIONS 256U

// ARM returns the current sequence for the next recheck/arm cycle.
// Readable/Writable also become ready on close: the corresponding operation
// can return a terminal status. Notifications are hints; retry the operation
// to distinguish queued data, available space, and closure.
#define MYOS_CHANNEL_READABLE 0U
#define MYOS_CHANNEL_WRITABLE 1U
#define MYOS_CHANNEL_PEER_CLOSED 2U

struct myos_channel_message {
    uint32_t version;
    uint32_t flags;
    uint32_t word_count;
    uint32_t cap_count;
    uint32_t receive_limit;
    uint32_t reserved;
    myos_word_t transaction;
    myos_word_t tag;
    myos_word_t words[MYOS_CHANNEL_MAX_WORDS];
    struct myos_cap_transfer caps[MYOS_CHANNEL_MAX_CAPS];
    myos_cap_t received[MYOS_CHANNEL_MAX_CAPS];
    uint32_t received_count;
    uint32_t received_reserved;
    myos_word_t sender_badge;
    myos_word_t sequence;
};

struct myos_channel_config {
    uint32_t version;
    uint32_t flags;
    uint32_t queue_capacity;
    uint32_t max_words;
    uint32_t max_caps;
    uint32_t waiter_capacity; // Legacy layout field; no per-Channel waiter quota.
    uint32_t relation_capacity;
    uint32_t reserved;
};
