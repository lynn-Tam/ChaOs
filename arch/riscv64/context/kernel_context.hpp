// RISC-V callee-saved kernel context shared with kernel_context.S.

#pragma once

#define RISCV64_KERNEL_CONTEXT_RA_OFFSET 0
#define RISCV64_KERNEL_CONTEXT_SP_OFFSET 8
#define RISCV64_KERNEL_CONTEXT_S0_OFFSET 16
#define RISCV64_KERNEL_CONTEXT_S1_OFFSET 24
#define RISCV64_KERNEL_CONTEXT_S2_OFFSET 32
#define RISCV64_KERNEL_CONTEXT_S3_OFFSET 40
#define RISCV64_KERNEL_CONTEXT_S4_OFFSET 48
#define RISCV64_KERNEL_CONTEXT_S5_OFFSET 56
#define RISCV64_KERNEL_CONTEXT_S6_OFFSET 64
#define RISCV64_KERNEL_CONTEXT_S7_OFFSET 72
#define RISCV64_KERNEL_CONTEXT_S8_OFFSET 80
#define RISCV64_KERNEL_CONTEXT_S9_OFFSET 88
#define RISCV64_KERNEL_CONTEXT_S10_OFFSET 96
#define RISCV64_KERNEL_CONTEXT_S11_OFFSET 104
#define RISCV64_KERNEL_CONTEXT_SIZE 112

#if !defined(__ASSEMBLER__)

#include <core/types.hpp>
#include <libk/typetraits.hpp>
#include <stddef.h>

namespace arch::riscv64 {

struct KernelContext final {
    usize ra{};
    usize sp{};
    usize s0{};
    usize s1{};
    usize s2{};
    usize s3{};
    usize s4{};
    usize s5{};
    usize s6{};
    usize s7{};
    usize s8{};
    usize s9{};
    usize s10{};
    usize s11{};
};

static_assert(libk::is_standard_layout_v<KernelContext>);
static_assert(libk::is_trivially_destructible_v<KernelContext>);
static_assert(sizeof(KernelContext) == RISCV64_KERNEL_CONTEXT_SIZE);
static_assert(alignof(KernelContext) == alignof(usize));
static_assert(offsetof(KernelContext, ra) == RISCV64_KERNEL_CONTEXT_RA_OFFSET);
static_assert(offsetof(KernelContext, sp) == RISCV64_KERNEL_CONTEXT_SP_OFFSET);
static_assert(offsetof(KernelContext, s0) == RISCV64_KERNEL_CONTEXT_S0_OFFSET);
static_assert(offsetof(KernelContext, s1) == RISCV64_KERNEL_CONTEXT_S1_OFFSET);
static_assert(offsetof(KernelContext, s2) == RISCV64_KERNEL_CONTEXT_S2_OFFSET);
static_assert(offsetof(KernelContext, s3) == RISCV64_KERNEL_CONTEXT_S3_OFFSET);
static_assert(offsetof(KernelContext, s4) == RISCV64_KERNEL_CONTEXT_S4_OFFSET);
static_assert(offsetof(KernelContext, s5) == RISCV64_KERNEL_CONTEXT_S5_OFFSET);
static_assert(offsetof(KernelContext, s6) == RISCV64_KERNEL_CONTEXT_S6_OFFSET);
static_assert(offsetof(KernelContext, s7) == RISCV64_KERNEL_CONTEXT_S7_OFFSET);
static_assert(offsetof(KernelContext, s8) == RISCV64_KERNEL_CONTEXT_S8_OFFSET);
static_assert(offsetof(KernelContext, s9) == RISCV64_KERNEL_CONTEXT_S9_OFFSET);
static_assert(offsetof(KernelContext, s10) == RISCV64_KERNEL_CONTEXT_S10_OFFSET);
static_assert(offsetof(KernelContext, s11) == RISCV64_KERNEL_CONTEXT_S11_OFFSET);

} // namespace arch::riscv64

#endif
