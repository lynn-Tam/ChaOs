#pragma once

/*
 * Public object-kind identifiers are part of the capability and deployment
 * ABI.  They are deliberately independent from any wire struct layout: a
 * reader must treat the values as a closed enum and reject unknown values.
 */
#ifndef __ASSEMBLER__
#include <stdint.h>

#ifdef __cplusplus
enum ObjKind : uint16_t {
#else
enum ObjKind {
#endif
    OBJECT_KIND_INVALID = 0,
    OBJECT_KIND_THREAD = 1,
    OBJECT_KIND_SCHED_CONTEXT = 2,
    OBJECT_KIND_SCHED_DOMAIN = 3,
    OBJECT_KIND_CSPACE = 4,
    OBJECT_KIND_MEMORY = 5,
    OBJECT_KIND_VSPACE = 6,
    OBJECT_KIND_RESOURCE_POOL = 7,
    OBJECT_KIND_NOTIFICATION = 8,
    /* 9-10 reserved: removed experimental lane and transport. */
    OBJECT_KIND_ENDPOINT = 11,
    OBJECT_KIND_CHANNEL = 12,
    OBJECT_KIND_PAGER = 13,
    OBJECT_KIND_IRQ = 14,
    OBJECT_KIND_DEVICE = 15,
    OBJECT_KIND_IO_SPACE = 16,
    OBJECT_KIND_COUNT = 17,
};

#define OBJECT_KINDS (((UINT64_C(1) << OBJECT_KIND_COUNT) - 2) & ~(UINT64_C(3) << 9))

typedef uint16_t obj_kind_t;

#ifdef __cplusplus
static_assert(sizeof(ObjKind) == sizeof(uint16_t));
#endif

#endif
