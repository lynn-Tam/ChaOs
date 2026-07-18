#pragma once

#include <stdint.h>
#include <uapi/capability.h>
#include <uapi/types.h>

#define MYOS_VPROC_START_VERSION 1U
#define MYOS_VPROC_RUNTIME_VERSION 1U
#define MYOS_VPROC_ARM_VERSION 1U
#define MYOS_VPROC_CONTEXT_WORDS 32U
#define MYOS_VPROC_MAX_OPERATIONS 32U
#define MYOS_VPROC_MAX_INGRESS 4U

typedef uint64_t myos_operation_key_t;

#define MYOS_OPERATION_SLOT_BITS 8U
#define MYOS_OPERATION_SLOT_MASK UINT64_C(0xff)

// Architecture-selected register image.  Word 0 is PC; the remaining words
// use the selected architecture's stable UAPI order.  Privileged status is
// deliberately absent and is reconstructed by the kernel on every resume.
struct myos_user_context {
    myos_word_t words[MYOS_VPROC_CONTEXT_WORDS];
};

// Immutable constructor snapshot read from an authorized MemoryObject.
// control/event mappings must already cover exactly one resident page in the
// target VSpace.  The kernel pins both mappings and backing grants for the
// complete Vproc lifetime.
struct myos_vproc_start {
    uint32_t version;
    uint32_t flags;
    myos_word_t entry;
    myos_word_t stack;
    myos_word_t arguments[6];
    myos_cap_t control_memory;
    myos_word_t control_page;
    myos_word_t control_address;
    myos_cap_t event_memory;
    myos_word_t event_page;
    myos_word_t event_address;
};

// Submitted by the current Vproc after bootstrap has initialized its runtime.
// The first implementation accepts one resident code page and one resident
// stack page; the range fields keep the ABI explicit and extensible.
struct myos_vproc_arm {
    uint32_t version;
    uint32_t flags;
    myos_word_t entry;
    myos_cap_t code_memory;
    myos_word_t code_page;
    myos_word_t code_address;
    myos_word_t code_pages;
    myos_cap_t stack_memory;
    myos_word_t stack_page;
    myos_word_t stack_address;
    myos_word_t stack_pages;
    myos_word_t stack_top;
};

// User-writable protocol page.  Atomic words are accessed with the acquire /
// release rules documented by the Vproc ABI; none of these fields is trusted
// as kernel-owned state.
struct myos_vproc_control_page {
    uint32_t version;
    uint32_t flags;
    myos_word_t upcall_disable_depth;
    myos_word_t observed_sequence;
    myos_word_t resume_generation;
    myos_word_t resume_flags;
    struct myos_user_context resume;
};

// User read-only projection.  Operation slots in the kernel remain canonical;
// ready_mask and the delivered context may be overwritten by a later event.
struct myos_vproc_event_page {
    uint32_t version;
    uint32_t flags;
    myos_word_t pending_sequence;
    myos_word_t active_generation;
    uint64_t ready_mask;
    uint64_t ingress_mask;
    uint64_t ingress_sequence[MYOS_VPROC_MAX_INGRESS];
    myos_word_t ingress_tag[MYOS_VPROC_MAX_INGRESS];
    struct myos_user_context delivered;
};
