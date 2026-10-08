#pragma once

/* The root execution has a resident, registered IPC page at this address. */
#define BOOT_ROOT_IPC_ADDRESS 0x40000000UL

#include <stdint.h>
#include <stddef.h>
#include <uapi/cap.h>
#include <uapi/ipc.h>
#include <uapi/abi.h>


#define BOOT_MAGIC UINT64_C(0x4d594f53494e4954)
#define BOOT_MAJOR 3U
#define BOOT_MINOR 0U

enum BootRole {
    BOOT_VSPACE = 1,
    BOOT_CSPACE = 2,
    BOOT_POOL = 3,
    BOOT_DOMAIN = 4,
    BOOT_SC = 5,
    BOOT_BUNDLE = 6,
    BOOT_THREAD = 7,
    BOOT_EVENTS = 10,
    BOOT_READY = 14,
    BOOT_IO_HOST = 21,
};

#ifdef __cplusplus
/*
 * Bootstrap entries are a closed ABI projection, not free-form labels.  Keep
 * the cap-role to object-kind relation next to the wire enum so manifest
 * admission and envelope construction share one source of truth.
 */
[[nodiscard]] constexpr auto boot_kind(uint32_t kind) noexcept -> obj_kind_t {
    switch (kind) {
    case BOOT_VSPACE:
        return OBJECT_KIND_VSPACE;
    case BOOT_CSPACE:
        return OBJECT_KIND_CSPACE;
    case BOOT_POOL:
        return OBJECT_KIND_RESOURCE_POOL;
    case BOOT_DOMAIN:
        return OBJECT_KIND_SCHED_DOMAIN;
    case BOOT_SC:
        return OBJECT_KIND_SCHED_CONTEXT;
    case BOOT_BUNDLE:
        return OBJECT_KIND_MEMORY;
    case BOOT_THREAD:
        return OBJECT_KIND_THREAD;
    case BOOT_IO_HOST:
        return OBJECT_KIND_IO_HOST;
    case BOOT_EVENTS:
        return OBJECT_KIND_NOTIFICATION;
    case BOOT_READY:
        return OBJECT_KIND_NOTIFICATION;
    default:
        return OBJECT_KIND_INVALID;
    }
}
#endif

// Entries follow the header, with no empty slots. role==0 names a user contract.
// Selectors name capabilities already installed in the receiving CSpace.
#define BOOT_NAME_MAX 32U
struct BootCap {
    uint32_t role;
    uint32_t protocol;
    uint16_t major;
    uint16_t minor;
    uint16_t kind;
    uint16_t reserved;
    cap_t handle;
    char name[BOOT_NAME_MAX];
    // Physical resource metadata; authority always comes from handle.
    uint64_t phys;
    uint64_t bytes;
};

struct BootHdr {
    uint64_t magic;
    uint16_t major;
    uint16_t minor;
    uint32_t size;
    uint32_t count;
    uint32_t cpu_count;
    uint64_t stack_base;
    uint64_t stack_size;
    uint64_t boot_bundle_size;
};

#ifdef __cplusplus
static_assert(sizeof(BootHdr) == 48);
static_assert(sizeof(BootCap) == 72);
#endif

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
