#pragma once

#define VM_READ    (1U << 0)
#define VM_WRITE   (1U << 1)
#define VM_EXECUTE (1U << 2)

// VM_MAP only: writes copy the source page into this mapping on first write.
#define VM_MAP_PRIVATE (1U << 8)

// Pager-backed private content may discard clean pages, but has no writeback
// destination. Dirty pages stay resident until their Mem is retired.
#define MEMORY_PAGER_PRIVATE (1U << 0)

#include <stdint.h>
#include <uapi/abi.h>

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
