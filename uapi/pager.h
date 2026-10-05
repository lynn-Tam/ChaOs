#pragma once

#include <stdint.h>
#include <uapi/types.h>

#define MYOS_PAGER_REQUEST_PAGE_IN 1U
#define MYOS_PAGER_REQUEST_WRITEBACK 2U

// Claim metadata is output only. Replies carry id directly in a register;
// the kernel owns the page generation and the submitted writeback snapshot.
struct myos_pager_request {
    uint32_t kind;
    uint32_t urgency;
    myos_word_t id;
    myos_word_t page_index;
    union {
        struct {
            myos_word_t first;
            myos_word_t count;
        } page_in;
        struct {
            myos_word_t dirty_epoch;
        } writeback;
    } payload;
};

#ifdef __cplusplus
static_assert(sizeof(myos_pager_request) == 40);
#endif
