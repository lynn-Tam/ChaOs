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
    ObjInvalid = 0,
    ObjThread = 1,
    ObjSc = 2,
    ObjDomain = 3,
    ObjCSpace = 4,
    ObjMem = 5,
    ObjVSpace = 6,
    ObjQuota = 7,
    ObjNotification = 8,
    ObjChannel = 9,
    ObjPager = 10,
    ObjIrq = 11,
    ObjIommu = 12,
    ObjIoSpace = 13,
    ObjPhys = 14,
    ObjIrqCtl = 15,
    ObjScope = 16,
    ObjPort = 17,
    ObjVmOp = 18,
    ObjIoOp = 19,
    ObjCount = 20,
};

#define OBJ_KINDS ((UINT64_C(1) << ObjCount) - 2)

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
    // VSpace v2: [base, bytes, perms, claim, 0, 0]. Typed attenuation keeps
    // the source claim; claim zero is administrative, not an acquired region.
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
#define CAP_ATTENUATION_VERSION_CURRENT 2U

/* Capability-family fields with a stable public encoding. */
#define CAP_CHANNEL_SIDE_A 0U
#define CAP_CHANNEL_SIDE_B 1U

#define RIGHT_DUPLICATE           (U64_C(1) << 0)
#define RIGHT_DELEGATE            (U64_C(1) << 1)
#define RIGHT_RESERVE             (U64_C(1) << 2)
#define RIGHT_MAP                 (U64_C(1) << 3)
#define RIGHT_UNMAP               (U64_C(1) << 4)
#define RIGHT_PROTECT             (U64_C(1) << 5)
#define RIGHT_DESTROY             (U64_C(1) << 6)
#define RIGHT_INSPECT             (U64_C(1) << 7)
#define RIGHT_CONTROL             (U64_C(1) << 8)
#define RIGHT_MANAGE              (U64_C(1) << 9)
#define RIGHT_REVOKE              (U64_C(1) << 10)
#define RIGHT_CREATE              (U64_C(1) << 11)
#define RIGHT_SPLIT               (U64_C(1) << 12)
#define RIGHT_CLOSE               (U64_C(1) << 13)
#define RIGHT_SIGNAL              (U64_C(1) << 14)
#define RIGHT_RECEIVE             (U64_C(1) << 15)
#define RIGHT_CONNECT             (U64_C(1) << 16)
#define RIGHT_ACK                 (U64_C(1) << 17)
#define RIGHT_SEND                (U64_C(1) << 18)
#define RIGHT_SERVE               (U64_C(1) << 19)
#define RIGHT_SUPPLY              (U64_C(1) << 20)
#define RIGHT_ROUTE               (U64_C(1) << 23)
#define RIGHT_OBSERVE             (U64_C(1) << 24)
#define RIGHT_ATTACH              (U64_C(1) << 25)
#define RIGHT_ADOPT               (U64_C(1) << 26)
#define RIGHT_REASSIGN              (U64_C(1) << 27)
#define RIGHT_MASK          ((U64_C(1) << 28) - 1)

#define OBJ_BIT(kind) (U64_C(1) << (kind))

#ifndef __ASSEMBLER__
#define OBJECT_OPEN 0U
#define OBJECT_CLOSING 1U
#define OBJECT_DRAINED 2U
#define STATE_OPEN 1U
#define STATE_CLOSING 2U
#define STATE_DRAINED 4U
struct CreateAuth { cap_t quota, scope; };
#define SCOPE_INFO 1U
#define SCOPE_MEMBERS 2U
#define SCOPE_CAPS 4U
#define SCOPE_LINKS 8U
#define SCOPE_USAGE 16U
#define SCOPE_OPS 32U
#define SCOPE_GRANTS 64U
#define SCOPE_FACETS 127U
struct ScopeInfo {
    uint32_t version, size;
    uint64_t members, epoch;
    uint32_t state, reserved;
};
// Cursors are bound to one source and one structural version.
struct ScopeCursor { uint64_t source, version, last; };
#define SCOPE_MEMBER_PAGE 8U
#define SCOPE_MEMBER_PENDING 0U
#define SCOPE_MEMBER_LIVE 1U
#define SCOPE_MEMBER_CLOSING 2U
struct ScopeMember {
    uint64_t object_id, epoch;
    uint32_t kind, state;
};
struct ScopeMembers {
    uint32_t version, size;
    struct ScopeCursor next;
    uint32_t count, more;
    struct ScopeMember records[SCOPE_MEMBER_PAGE];
};
#define OBS_BOUNDARY 1U
#define LINK_RECORD_PAGE 8U
#define LINK_LIVE 1U
#define LINK_INVALIDATING 2U
#define LINK_DRAINING 3U
#define LINK_VM 1U
#define LINK_IO 2U
#define LINK_ROOT 3U
#define LINK_BIND 4U
#define LINK_CPU 5U
#define LINK_PARENT 6U
#define LINK_USE 7U
#define LINK_ROUTE 8U
#define LINK_PHYS 9U
struct LinkRecord {
    uint64_t link_id, object_id, grant_id, first, bytes, offset;
    uint32_t kind, state, perms, flags;
};
struct LinkRecords {
    uint32_t version, size;
    uint64_t epoch, cursor;
    uint32_t count, more;
    struct LinkRecord records[LINK_RECORD_PAGE];
};
#define GRANT_RECORD_PAGE 8U
#define OBS_TRUNCATED 2U
struct GrantQuery { cap_t selector; uint64_t selected, last, epoch; };
struct GrantRecord {
    uint64_t grant_id, parent_id, object_id;
    uint32_t state, flags;
    struct CapView ceiling;
};
struct GrantRecords {
    uint32_t version, size;
    uint64_t seed, selected, last, epoch;
    uint32_t count, more, flags, reserved;
    struct GrantRecord root, records[GRANT_RECORD_PAGE];
};
struct ScopeObject {
    uint32_t version, size;
    uint64_t object_id, owner_id, owner_epoch, quota_id, quota_epoch;
    uint64_t storage_memory, storage_caps;
    uint32_t kind, gate, flags, reserved;
};
struct ObjectInfo {
    uint32_t version, size;
    uint64_t object_id, quota_epoch;
    uint32_t kind, gate; // Common Object gate; payload-specific state uses typed info.
};
struct QuotaInfo {
    uint32_t version, size;
    uint64_t memory_limit, memory_available, cap_limit, caps_available;
    uint64_t allocations, reservations;
    uint32_t state, reserved;
};
#define CAP_REVOKE_ASYNC 1U
#define CAP_INFO_VALID 1U
#define CAP_INFO_UNAVAILABLE 2U
#define CAP_INFO_BOUNDARY 4U
#define CAP_INFO_PAGE 8U
#define CSPACE_INFO_VERSION 3U
struct CapInfo {
    cap_t selector;
    uint64_t object_id, grant_id;
    uint32_t state, reserved;
    struct CapView view;
};
#ifdef __cplusplus
static_assert(sizeof(CapInfo) == 96);
static_assert(offsetof(CapInfo, view) == 32);
#endif
struct CSpaceInfo {
    uint32_t version, size;
    uint64_t slots, pages, epoch;
    cap_t cursor;
    uint32_t count, reserved;
    struct CapInfo records[CAP_INFO_PAGE];
};
#endif

// Operation facts. Cancel requests do not imply the provider is drained.
#define OP_ACCEPTED  (1U << 0)
#define OP_COMMITTED (1U << 1)
#define OP_DRAINING  (1U << 2)
#define OP_DONE      (1U << 3)
