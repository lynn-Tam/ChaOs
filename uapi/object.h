#pragma once

/*
 * Public object-kind identifiers are part of the capability and deployment
 * ABI.  They are deliberately independent from any wire struct layout: a
 * reader must treat the values as a closed enum and reject unknown values.
 */
#ifndef __ASSEMBLER__
#include <stdint.h>

#ifdef __cplusplus
enum myos_object_kind : uint16_t {
#else
enum myos_object_kind {
#endif
    MYOS_OBJECT_KIND_INVALID = 0,
    MYOS_OBJECT_KIND_THREAD = 1,
    MYOS_OBJECT_KIND_SCHED_CONTEXT = 2,
    MYOS_OBJECT_KIND_SCHED_DOMAIN = 3,
    MYOS_OBJECT_KIND_CSPACE = 4,
    MYOS_OBJECT_KIND_MEMORY = 5,
    MYOS_OBJECT_KIND_VSPACE = 6,
    MYOS_OBJECT_KIND_RESOURCE_POOL = 7,
    MYOS_OBJECT_KIND_NOTIFICATION = 8,
    /* 9-10 reserved: removed experimental lane and transport. */
    MYOS_OBJECT_KIND_ENDPOINT = 11,
    MYOS_OBJECT_KIND_CHANNEL = 12,
    MYOS_OBJECT_KIND_PAGER = 13,
    MYOS_OBJECT_KIND_IRQ = 14,
    MYOS_OBJECT_KIND_DEVICE = 15,
    MYOS_OBJECT_KIND_IO_SPACE = 16,
    MYOS_OBJECT_KIND_COUNT = 17,
};

#define MYOS_OBJECT_KINDS (((UINT64_C(1) << MYOS_OBJECT_KIND_COUNT) - 2) & ~(UINT64_C(3) << 9))

typedef uint16_t myos_object_kind_t;

#ifdef __cplusplus
static_assert(sizeof(myos_object_kind) == sizeof(uint16_t));
#endif

#endif
