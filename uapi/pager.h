#pragma once

#include <stdint.h>
#include <uapi/types.h>

#define PAGER_REQUEST_PAGE_IN 1U
#define PAGER_REQUEST_WRITEBACK 2U

// Claim metadata is output only. Replies carry id directly in a register;
// the kernel owns the page generation and the submitted writeback snapshot.
struct PagerReq {
    uint32_t kind;
    uint32_t urgency;
    word_t id;
    word_t page_index;
    union {
        struct {
            word_t first;
            word_t count;
        } page_in;
        struct {
            word_t dirty_epoch;
        } writeback;
    } payload;
};

#ifdef __cplusplus
static_assert(sizeof(PagerReq) == 40);
#endif
