#pragma once

#ifdef __ASSEMBLER__
#define MYOS_U64_C(value) value
#else
#include <stddef.h>
#include <stdint.h>

typedef uint64_t myos_cap_t;
#define MYOS_U64_C(value) UINT64_C(value)
#endif

/*
 * A fixed-width, naturally aligned source-relative attenuation descriptor.
 * This is an ABI shape only: syscall consumers snapshot and decode each
 * field explicitly as little-endian bytes rather than treating a user buffer
 * as a native C object.
 */
#ifndef __ASSEMBLER__
#ifdef __cplusplus
struct alignas(8) myos_cap_attenuation {
#else
struct myos_cap_attenuation {
#endif
    uint16_t version;
    uint16_t kind;
    uint32_t size;
    uint64_t rights;
    uint64_t words[6];
};

#ifdef __cplusplus
static_assert(sizeof(myos_cap_attenuation) == 64);
static_assert(alignof(myos_cap_attenuation) == 8);
static_assert(offsetof(myos_cap_attenuation, version) == 0);
static_assert(offsetof(myos_cap_attenuation, kind) == 2);
static_assert(offsetof(myos_cap_attenuation, size) == 4);
static_assert(offsetof(myos_cap_attenuation, rights) == 8);
static_assert(offsetof(myos_cap_attenuation, words) == 16);
#endif
#endif

#define MYOS_CAP_ATTENUATION_VERSION_OFFSET 0U
#define MYOS_CAP_ATTENUATION_KIND_OFFSET 2U
#define MYOS_CAP_ATTENUATION_SIZE_OFFSET 4U
#define MYOS_CAP_ATTENUATION_RIGHTS_OFFSET 8U
#define MYOS_CAP_ATTENUATION_WORD0_OFFSET 16U
#define MYOS_CAP_ATTENUATION_WORD1_OFFSET 24U
#define MYOS_CAP_ATTENUATION_WORD2_OFFSET 32U
#define MYOS_CAP_ATTENUATION_WORD3_OFFSET 40U
#define MYOS_CAP_ATTENUATION_WORD4_OFFSET 48U
#define MYOS_CAP_ATTENUATION_WORD5_OFFSET 56U
#define MYOS_CAP_ATTENUATION_SIZE 64U
#define MYOS_CAP_ATTENUATION_VERSION_CURRENT 1U

/* Capability-family fields with a stable public encoding. */
#define MYOS_CAP_CHANNEL_SIDE_A 0U
#define MYOS_CAP_CHANNEL_SIDE_B 1U

#define MYOS_RIGHT_DUPLICATE     (MYOS_U64_C(1) << 0)
#define MYOS_RIGHT_DELEGATE      (MYOS_U64_C(1) << 1)
#define MYOS_RIGHT_RESERVE       (MYOS_U64_C(1) << 2)
#define MYOS_RIGHT_CREATE_REGION (MYOS_U64_C(1) << 3)
#define MYOS_RIGHT_MAP           (MYOS_U64_C(1) << 4)
#define MYOS_RIGHT_UNMAP         (MYOS_U64_C(1) << 5)
#define MYOS_RIGHT_PROTECT       (MYOS_U64_C(1) << 6)
#define MYOS_RIGHT_DESTROY       (MYOS_U64_C(1) << 7)
#define MYOS_RIGHT_INSPECT       (MYOS_U64_C(1) << 8)
#define MYOS_RIGHT_CONTROL       (MYOS_U64_C(1) << 9)
#define MYOS_RIGHT_MANAGE        (MYOS_U64_C(1) << 10)
#define MYOS_RIGHT_REVOKE        (MYOS_U64_C(1) << 11)
#define MYOS_RIGHT_CREATE        (MYOS_U64_C(1) << 12)
#define MYOS_RIGHT_SPLIT         (MYOS_U64_C(1) << 13)
#define MYOS_RIGHT_CLOSE         (MYOS_U64_C(1) << 14)
#define MYOS_RIGHT_SIGNAL        (MYOS_U64_C(1) << 15)
#define MYOS_RIGHT_RECEIVE       (MYOS_U64_C(1) << 16)
#define MYOS_RIGHT_CONNECT       (MYOS_U64_C(1) << 17)
#define MYOS_RIGHT_ACK           (MYOS_U64_C(1) << 18)
#define MYOS_RIGHT_CALL          (MYOS_U64_C(1) << 19)
#define MYOS_RIGHT_SEND          (MYOS_U64_C(1) << 20)
#define MYOS_RIGHT_SERVE         (MYOS_U64_C(1) << 21)
#define MYOS_RIGHT_SUPPLY        (MYOS_U64_C(1) << 22)
#define MYOS_RIGHT_FAIL          (MYOS_U64_C(1) << 23)
#define MYOS_RIGHT_WRITEBACK_ACK (MYOS_U64_C(1) << 24)
#define MYOS_RIGHT_ROUTE         (MYOS_U64_C(1) << 25)
#define MYOS_RIGHT_OBSERVE       (MYOS_U64_C(1) << 26)
#define MYOS_RIGHT_ATTACH        (MYOS_U64_C(1) << 27)
#define MYOS_RIGHT_MASK          ((MYOS_U64_C(1) << 28) - 1)
