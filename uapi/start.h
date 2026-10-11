#pragma once

/* The root execution has a resident, registered IPC page at this address. */
#define BOOT_ROOT_IPC_ADDRESS 0x40000000UL

#include <stdint.h>
#include <stddef.h>
#include <uapi/cap.h>
#include <uapi/ipc.h>
#include <uapi/abi.h>


#define BOOT_MAGIC UINT64_C(0x4d594f53494e4954)
#define BOOT_MAJOR 4U
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
    BOOT_FIRMWARE = 22,
    BOOT_RESOURCE = 23,
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
        return ObjVSpace;
    case BOOT_CSPACE:
        return ObjCSpace;
    case BOOT_POOL:
        return ObjGroup;
    case BOOT_DOMAIN:
        return ObjDomain;
    case BOOT_SC:
        return ObjSc;
    case BOOT_FIRMWARE:
    case BOOT_BUNDLE:
        return ObjMem;
    case BOOT_THREAD:
        return ObjThread;
    case BOOT_IO_HOST:
        return ObjHost;
    case BOOT_EVENTS:
        return ObjNotification;
    case BOOT_READY:
        return ObjNotification;
    default:
        return ObjInvalid;
    }
}
#endif

// role==0 is a userspace contract; BOOT_RESOURCE identifies a firmware node's
// resource by object kind and index. Metadata never grants hardware access.
#define BOOT_NAME_MAX 32U
#define FW_FDT 1U
struct BootContract {
    uint32_t protocol;
    uint16_t major, minor;
    char name[BOOT_NAME_MAX];
};
struct BootCap {
    uint32_t role;
    uint16_t kind, reserved;
    cap_t handle;
    union {
        struct BootContract contract;
        struct { uint32_t node, index; } resource;
        struct { uint32_t format; } firmware;
    };
    uint64_t phys, bytes;
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
