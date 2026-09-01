#pragma once

#include <stdint.h>
#include <uapi/capability.h>

#ifndef __ASSEMBLER__
#include <uapi/object.h>
#endif

#define MYOS_BOOTSTRAP_MAGIC UINT64_C(0x4d594f53494e4954)
#define MYOS_BOOTSTRAP_MAJOR 1U
#define MYOS_BOOTSTRAP_MINOR 4U
#define MYOS_BOOTSTRAP_MAX_CAPS 16U

enum myos_bootstrap_cap_kind {
    MYOS_BOOTSTRAP_CAP_VSPACE = 1,
    MYOS_BOOTSTRAP_CAP_CSPACE = 2,
    MYOS_BOOTSTRAP_CAP_RESOURCE_POOL = 3,
    MYOS_BOOTSTRAP_CAP_SCHED_DOMAIN = 4,
    MYOS_BOOTSTRAP_CAP_SCHED_CONTEXT = 5,
    MYOS_BOOTSTRAP_CAP_BOOT_BUNDLE = 6,
    MYOS_BOOTSTRAP_CAP_THREAD = 7,
    MYOS_BOOTSTRAP_CAP_DEVICE_MEMORY = 8,
    MYOS_BOOTSTRAP_CAP_IRQ = 9,
    MYOS_BOOTSTRAP_CAP_SERVICE_NOTIFICATION = 10,
    MYOS_BOOTSTRAP_CAP_PAGER = 11,
    MYOS_BOOTSTRAP_CAP_TARGET_MEMORY = 12,
    MYOS_BOOTSTRAP_CAP_STAGING_MEMORY = 13,
    MYOS_BOOTSTRAP_CAP_READINESS_NOTIFICATION = 14,
    MYOS_BOOTSTRAP_CAP_STAGING_REGION = 15,
};

#ifdef __cplusplus
/*
 * Bootstrap entries are a closed ABI projection, not free-form labels.  Keep
 * the cap-role to object-kind relation next to the wire enum so manifest
 * admission and envelope construction share one source of truth.
 */
[[nodiscard]] constexpr auto myos_bootstrap_object_kind(
    uint32_t kind) noexcept -> myos_object_kind_t {
    switch (kind) {
    case MYOS_BOOTSTRAP_CAP_VSPACE:
        return MYOS_OBJECT_KIND_VSPACE;
    case MYOS_BOOTSTRAP_CAP_CSPACE:
        return MYOS_OBJECT_KIND_CSPACE;
    case MYOS_BOOTSTRAP_CAP_RESOURCE_POOL:
        return MYOS_OBJECT_KIND_RESOURCE_POOL;
    case MYOS_BOOTSTRAP_CAP_SCHED_DOMAIN:
        return MYOS_OBJECT_KIND_SCHED_DOMAIN;
    case MYOS_BOOTSTRAP_CAP_SCHED_CONTEXT:
        return MYOS_OBJECT_KIND_SCHED_CONTEXT;
    case MYOS_BOOTSTRAP_CAP_BOOT_BUNDLE:
        return MYOS_OBJECT_KIND_MEMORY;
    case MYOS_BOOTSTRAP_CAP_THREAD:
        return MYOS_OBJECT_KIND_THREAD;
    case MYOS_BOOTSTRAP_CAP_DEVICE_MEMORY:
        return MYOS_OBJECT_KIND_MEMORY;
    case MYOS_BOOTSTRAP_CAP_IRQ:
        return MYOS_OBJECT_KIND_IRQ;
    case MYOS_BOOTSTRAP_CAP_SERVICE_NOTIFICATION:
        return MYOS_OBJECT_KIND_NOTIFICATION;
    case MYOS_BOOTSTRAP_CAP_PAGER:
        return MYOS_OBJECT_KIND_PAGER;
    case MYOS_BOOTSTRAP_CAP_TARGET_MEMORY:
    case MYOS_BOOTSTRAP_CAP_STAGING_MEMORY:
        return MYOS_OBJECT_KIND_MEMORY;
    case MYOS_BOOTSTRAP_CAP_READINESS_NOTIFICATION:
        return MYOS_OBJECT_KIND_NOTIFICATION;
    case MYOS_BOOTSTRAP_CAP_STAGING_REGION:
        return MYOS_OBJECT_KIND_VSPACE;
    default:
        return MYOS_OBJECT_KIND_INVALID;
    }
}
#endif

struct myos_bootstrap_cap {
    uint32_t kind;
    uint32_t flags;
    myos_cap_t handle;
};

struct myos_bootstrap_info {
    uint64_t magic;
    uint16_t major;
    uint16_t minor;
    uint32_t size;
    uint32_t cap_count;
    uint32_t cpu_count;
    uintptr_t stack_base;
    uint64_t stack_size;
    uint64_t boot_bundle_size;
    struct myos_bootstrap_cap caps[MYOS_BOOTSTRAP_MAX_CAPS];
};
