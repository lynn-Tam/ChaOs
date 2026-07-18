#pragma once

#include <stdint.h>

// Typed constructor authority mask. These values are ABI identifiers, not
// generic object_create tags: each admitted kind still has a distinct syscall.
#define MYOS_RESOURCE_THREAD            (UINT64_C(1) << 1)
#define MYOS_RESOURCE_SCHED_CONTEXT     (UINT64_C(1) << 2)
#define MYOS_RESOURCE_SCHED_DOMAIN      (UINT64_C(1) << 3)
#define MYOS_RESOURCE_CSPACE            (UINT64_C(1) << 4)
#define MYOS_RESOURCE_MEMORY            (UINT64_C(1) << 5)
#define MYOS_RESOURCE_VSPACE            (UINT64_C(1) << 6)
#define MYOS_RESOURCE_POOL              (UINT64_C(1) << 7)
#define MYOS_RESOURCE_NOTIFICATION      (UINT64_C(1) << 8)
#define MYOS_RESOURCE_VPROC             (UINT64_C(1) << 9)
#define MYOS_RESOURCE_TUNNEL            (UINT64_C(1) << 10)

#define MYOS_RESOURCE_E1_KINDS ( \
    MYOS_RESOURCE_THREAD | MYOS_RESOURCE_SCHED_CONTEXT | \
    MYOS_RESOURCE_CSPACE | MYOS_RESOURCE_MEMORY | \
    MYOS_RESOURCE_VSPACE | MYOS_RESOURCE_POOL)

#define MYOS_RESOURCE_E2_KINDS ( \
    MYOS_RESOURCE_E1_KINDS | MYOS_RESOURCE_NOTIFICATION)

#define MYOS_RESOURCE_E3_KINDS ( \
    MYOS_RESOURCE_E2_KINDS | MYOS_RESOURCE_VPROC | MYOS_RESOURCE_TUNNEL)
