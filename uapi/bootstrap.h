#pragma once

#include <stdint.h>
#include <uapi/capability.h>

#define MYOS_BOOTSTRAP_MAGIC UINT64_C(0x4d594f53494e4954)
#define MYOS_BOOTSTRAP_MAJOR 1U
#define MYOS_BOOTSTRAP_MINOR 2U
#define MYOS_BOOTSTRAP_MAX_CAPS 16U

enum myos_bootstrap_cap_kind {
    MYOS_BOOTSTRAP_CAP_VSPACE = 1,
    MYOS_BOOTSTRAP_CAP_CSPACE = 2,
    MYOS_BOOTSTRAP_CAP_RESOURCE_POOL = 3,
    MYOS_BOOTSTRAP_CAP_SCHED_DOMAIN = 4,
    MYOS_BOOTSTRAP_CAP_SCHED_CONTEXT = 5,
    MYOS_BOOTSTRAP_CAP_BOOT_BUNDLE = 6,
    MYOS_BOOTSTRAP_CAP_THREAD = 7,
};

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
