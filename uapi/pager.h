#pragma once

#include <stdint.h>
#include <uapi/types.h>

#define MYOS_PAGER_REQUEST_VERSION 1U
#define MYOS_PAGER_REQUEST_FLAGS_NONE 0U
#define MYOS_PAGER_REQUEST_PAGE_IN 1U
#define MYOS_PAGER_REQUEST_WRITEBACK 2U

// Exact pager request/reply identity carried in the registered IPC buffer. The
// syscall itself stays within the six a0-a5 argument registers; the descriptor
// is snapshotted before any ownership transition.
struct myos_pager_request {
    uint32_t version;
    uint32_t kind;
    uint32_t flags; /* v1: MYOS_PAGER_REQUEST_FLAGS_NONE */
    uint32_t urgency;
    myos_word_t delivery_slot;
    myos_word_t delivery_generation;
    myos_word_t claim_generation;
    myos_word_t page_generation;
    myos_word_t page_index;
    union {
        struct {
            myos_word_t first;
            myos_word_t count;
            myos_word_t backing_epoch;
            myos_word_t content_epoch;
        } page_in;
        struct {
            myos_word_t writeback_generation;
            myos_word_t dirty_epoch;
        } writeback;
    } payload;
};
