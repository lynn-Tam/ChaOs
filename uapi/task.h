#pragma once

#include <uapi/abi.h>
#include <uapi/ipc.h>

#define THREAD_START_VERSION 2U

// Immutable constructor snapshot read from an authorized MemoryObject. The
// kernel never follows a transient user pointer while building a Thread.
struct ThreadInit {
    uint32_t version;
    uint32_t flags;
    word_t entry;
    word_t stack;
    word_t arguments[6];
    struct IpcBinding ipc;
};

#ifdef __cplusplus
static_assert(sizeof(ThreadInit) == 104);
#endif

#define EXIT_NORMAL 1U
#define EXIT_FAULT 2U
#define EXIT_STOP 3U
// ThreadExit.kind: architecture-independent terminal fault causes.
// Normal exit and administrative Stop use FAULT_NONE.
#define FAULT_NONE       0U
#define FAULT_UNKNOWN    1U
#define FAULT_UNMAPPED   2U
#define FAULT_GUARD      3U
#define FAULT_PERMS      4U  // VSpace permission denial.
#define FAULT_QUOTA      5U  // Logical resource capacity exhausted.
#define FAULT_MEMORY     6U  // Physical memory allocation failed.
#define FAULT_BACKING    7U
#define FAULT_BREAKPOINT 8U
#define FAULT_ILLEGAL    9U
#define FAULT_ALIGN      10U
#define FAULT_ACCESS     11U // Architectural instruction/load/store access fault.
struct ThreadExit {
    uint32_t origin, kind;
    int32_t status;
    uint32_t reserved;
    uint64_t pc, address;
};
#define EXEC_FAILED 1U
#define EXEC_STOPPING 2U
#define EXEC_STOPPED 4U
struct ExecInfo {
    uint32_t version, size;
    uint64_t flags, sequence, thread;
    struct ThreadExit failure;
};

#define SC_BOUND 1U
#define SC_WITHDRAWING 2U

#define THREAD_ENABLED  (1U << 0)
#define THREAD_APPLIED  (1U << 1)
#define THREAD_RUNNING  (1U << 2)
#define THREAD_WAITING  (1U << 3)
#define THREAD_BOUND    (1U << 4)
#define THREAD_STOPPING (1U << 5)
#define THREAD_STOPPED  (1U << 6)
#define THREAD_MIGRATING (1U << 7)

// All time fields use the monotonic clock's ticks, including SC cap/debt.
struct ThreadInfo {
    uint32_t version, size;
    uint64_t requested, applied, runtime, association;
    uint32_t flags, cpu;
    uint64_t home_epoch;
    uint32_t destination, reserved;
};
struct ScInfo {
    uint32_t version, size;
    uint64_t generation, applied, cpus, runtime;
    uint64_t budget, period, available, debt;
    uint32_t weight, bound;
};

#define DOMAIN_OPEN 0U
#define DOMAIN_CLOSING 1U
#define DOMAIN_DRAINED 2U
#define DOMAIN_CONFIGURING 4U
#define DOMAIN_ENABLED 1U
#define DOMAIN_REQUESTED_ENABLED 2U
struct DomainInfo {
    uint32_t version, size;
    uint64_t cpus, budget, period, available, debt, escrow;
    uint32_t weight, phase;
    uint64_t requested, applied, runtime, work_runtime;
    int32_t status;
    uint32_t flags;
};
