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
    OBJECT_KIND_IO_HOST = 15,
    OBJECT_KIND_IO_SPACE = 16,
    OBJECT_KIND_COUNT = 17,
};

#define OBJECT_KINDS (((UINT64_C(1) << OBJECT_KIND_COUNT) - 2) & ~(UINT64_C(3) << 9))

typedef uint16_t obj_kind_t;

#ifdef __cplusplus
static_assert(sizeof(ObjKind) == sizeof(uint16_t));
#endif

#endif

#ifdef __ASSEMBLER__
#define U64_C(value) value
#else
#include <stddef.h>
#include <stdint.h>

typedef uint64_t cap_t;
#define U64_C(value) UINT64_C(value)
#endif

/*
 * A fixed-width, naturally aligned source-relative attenuation descriptor.
 * This is an ABI shape only: syscall consumers snapshot and decode each
 * field explicitly as little-endian bytes rather than treating a user buffer
 * as a native C object.
 */
#ifndef __ASSEMBLER__
#ifdef __cplusplus
struct alignas(8) CapView {
#else
struct CapView {
#endif
    uint16_t version;
    uint16_t kind;
    uint32_t size;
    uint64_t rights;
    uint64_t words[6];
};

#ifdef __cplusplus
static_assert(sizeof(CapView) == 64);
static_assert(alignof(CapView) == 8);
static_assert(offsetof(CapView, version) == 0);
static_assert(offsetof(CapView, kind) == 2);
static_assert(offsetof(CapView, size) == 4);
static_assert(offsetof(CapView, rights) == 8);
static_assert(offsetof(CapView, words) == 16);
#endif
#endif

#define CAP_ATTENUATION_VERSION_OFFSET 0U
#define CAP_ATTENUATION_KIND_OFFSET 2U
#define CAP_ATTENUATION_SIZE_OFFSET 4U
#define CAP_ATTENUATION_RIGHTS_OFFSET 8U
#define CAP_ATTENUATION_WORD0_OFFSET 16U
#define CAP_ATTENUATION_WORD1_OFFSET 24U
#define CAP_ATTENUATION_WORD2_OFFSET 32U
#define CAP_ATTENUATION_WORD3_OFFSET 40U
#define CAP_ATTENUATION_WORD4_OFFSET 48U
#define CAP_ATTENUATION_WORD5_OFFSET 56U
#define CAP_ATTENUATION_SIZE 64U
#define CAP_ATTENUATION_VERSION_CURRENT 1U

/* Capability-family fields with a stable public encoding. */
#define CAP_CHANNEL_SIDE_A 0U
#define CAP_CHANNEL_SIDE_B 1U

#define RIGHT_DUPLICATE     (U64_C(1) << 0)
#define RIGHT_DELEGATE      (U64_C(1) << 1)
#define RIGHT_RESERVE       (U64_C(1) << 2)
#define RIGHT_MAP           (U64_C(1) << 4)
#define RIGHT_UNMAP         (U64_C(1) << 5)
#define RIGHT_PROTECT       (U64_C(1) << 6)
#define RIGHT_DESTROY       (U64_C(1) << 7)
#define RIGHT_INSPECT       (U64_C(1) << 8)
#define RIGHT_CONTROL       (U64_C(1) << 9)
#define RIGHT_MANAGE        (U64_C(1) << 10)
#define RIGHT_REVOKE        (U64_C(1) << 11)
#define RIGHT_CREATE        (U64_C(1) << 12)
#define RIGHT_SPLIT         (U64_C(1) << 13)
#define RIGHT_CLOSE         (U64_C(1) << 14)
#define RIGHT_SIGNAL        (U64_C(1) << 15)
#define RIGHT_RECEIVE       (U64_C(1) << 16)
#define RIGHT_CONNECT       (U64_C(1) << 17)
#define RIGHT_ACK           (U64_C(1) << 18)
#define RIGHT_CALL          (U64_C(1) << 19)
#define RIGHT_SEND          (U64_C(1) << 20)
#define RIGHT_SERVE         (U64_C(1) << 21)
#define RIGHT_SUPPLY        (U64_C(1) << 22)
#define RIGHT_FAIL          (U64_C(1) << 23)
#define RIGHT_WRITEBACK_ACK (U64_C(1) << 24)
#define RIGHT_ROUTE         (U64_C(1) << 25)
#define RIGHT_OBSERVE       (U64_C(1) << 26)
#define RIGHT_ATTACH        (U64_C(1) << 27)
#define RIGHT_MASK          ((U64_C(1) << 28) - 1)

#define OBJ_BIT(kind) (U64_C(1) << (kind))
