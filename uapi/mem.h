#pragma once
#include <uapi/cap.h>

#define VM_READ    (1U << 0)
#define VM_WRITE   (1U << 1)
#define VM_EXECUTE (1U << 2)

// Native anonymous defaults may be materialized locally until content is evicted.
#define MEMORY_PAGER_ANON (1U << 0)
#define PAGER_SUPPLY_COPY (1U << 0)
#define PAGER_SUPPLY_NOWAIT (1U << 1) /* shortage does not enqueue Capacity */
#define PAGER_FAIL_RETRY (1U << 0) /* PageIn returns to Absent; wake for retry */


#include <stdint.h>
#include <uapi/abi.h>

#define VM_ALLOC_VERSION 1U
struct VmAlloc {
    uint32_t version, size;
    uint64_t address, bytes, alignment, lower, upper, perms;
};

// Accepted operations keep their own lifetime; closing the result slot does not cancel.
#define VM_OP_MAP 1U
#define VM_OP_UNMAP 2U /* holes are already unmapped; reservations/guards require CLEAR */
#define VM_OP_PROTECT 3U
#define VM_OP_CLEAR 4U
struct VmReq {
    cap_t memory;
    uint64_t address, bytes, page, perms;
};

#define PAGER_REQUEST_PAGE_IN 1U
#define PAGER_REQUEST_WRITEBACK 2U
#define PAGER_REQUEST_CAPACITY 3U
#define PAGER_REQUEST_RECLAIMED 4U

// Mem prepays initial page/request records. Further records grow within its
// Quota; transient shortages issue one coalesced Capacity request. Replies
// carry id directly; request ids and object keys never encode a pointer.
struct PagerReq {
    uint32_t kind;
    uint32_t reserved;
    word_t id, key;
    word_t page_index;
    union {
        struct {
            word_t first;
            word_t count;
        } page_in;
        struct {
            word_t dirty_epoch;
        } writeback;
        struct { word_t status, pages; } reclaimed;
        struct { word_t quota; } capacity; /* zero: physical pressure */
    } payload;
};

#ifdef __cplusplus
static_assert(sizeof(PagerReq) == 48);
#endif

#define VSPACE_LIVE 0U
#define VSPACE_STOPPING 1U
#define VSPACE_QUIESCENT 2U
struct VSpaceInfo {
    uint32_t version, size;
    uint64_t mapped, resident;
    uint32_t state, reserved;
};

// Settled CPU ticks of all executions rooted here, including exited threads.
struct VSpaceRuntime {
    uint32_t version, size;
    uint64_t runtime, sampled;
};

#define VM_RECORD_MAP 0U
#define VM_RECORD_RESERVED 1U
#define VM_RECORD_GUARD 2U
#define VM_RECORD_COUNT 16U
struct VmRecord {
    uint64_t address, bytes, resident, claim;
    uint32_t perms, kind;
};
struct VmRecords {
    uint32_t version, size;
    uint64_t cursor;
    uint32_t count, reserved;
    struct VmRecord records[VM_RECORD_COUNT];
};

#define PAGER_RESIDENT 1U
#define PAGER_DIRTY 2U
#define PAGER_BUSY 4U
#define PAGER_PINNED 8U
#define PAGER_ACCESSED 16U
#define PAGER_PARTIAL 32U
#define PAGER_RECORD_COUNT 32U
struct PagerPage { uint64_t page, epoch; uint32_t flags, reserved; };
struct PagerRecords {
    uint32_t version, size;
    uint64_t key, next_key, bytes, quota, cursor, alias_cursor, map_cursor;
    uint32_t count, state;
    struct PagerPage pages[PAGER_RECORD_COUNT];
};

struct MemoryInfo {
    uint32_t version, size;
    uint64_t total, free, reserve, sequence;
    uint32_t pressure, reserved;
};

struct PagerInfo { uint32_t version, size; uint64_t logical, limit, generation, objects; };
