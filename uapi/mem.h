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
    uint32_t reserved;
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

#define VSPACE_LIVE 0U
#define VSPACE_STOPPING 1U
#define VSPACE_QUIESCENT 2U
struct VSpaceInfo {
    uint32_t version, size;
    uint64_t mapped, resident;
    uint32_t state, reserved;
};

#define VM_RECORD_MAP 0U
#define VM_RECORD_RESERVED 1U
#define VM_RECORD_GUARD 2U
#define VM_RECORD_COUNT 16U
struct VmRecord {
    uint64_t address, bytes, resident;
    uint32_t perms, kind;
};
struct VmRecords {
    uint32_t version, size;
    uint64_t cursor;
    uint32_t count, reserved;
    struct VmRecord records[VM_RECORD_COUNT];
};
