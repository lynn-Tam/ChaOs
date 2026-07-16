#include "arch/riscv64/context/kernel_context.hpp"

.section .text, "ax", @progbits
.balign 4

.global arch_riscv64_switch_context
.hidden arch_riscv64_switch_context
.type arch_riscv64_switch_context, @function
arch_riscv64_switch_context:
    sd ra, RISCV64_KERNEL_CONTEXT_RA_OFFSET(a0)
    sd sp, RISCV64_KERNEL_CONTEXT_SP_OFFSET(a0)
    sd s0, RISCV64_KERNEL_CONTEXT_S0_OFFSET(a0)
    sd s1, RISCV64_KERNEL_CONTEXT_S1_OFFSET(a0)
    sd s2, RISCV64_KERNEL_CONTEXT_S2_OFFSET(a0)
    sd s3, RISCV64_KERNEL_CONTEXT_S3_OFFSET(a0)
    sd s4, RISCV64_KERNEL_CONTEXT_S4_OFFSET(a0)
    sd s5, RISCV64_KERNEL_CONTEXT_S5_OFFSET(a0)
    sd s6, RISCV64_KERNEL_CONTEXT_S6_OFFSET(a0)
    sd s7, RISCV64_KERNEL_CONTEXT_S7_OFFSET(a0)
    sd s8, RISCV64_KERNEL_CONTEXT_S8_OFFSET(a0)
    sd s9, RISCV64_KERNEL_CONTEXT_S9_OFFSET(a0)
    sd s10, RISCV64_KERNEL_CONTEXT_S10_OFFSET(a0)
    sd s11, RISCV64_KERNEL_CONTEXT_S11_OFFSET(a0)

    ld ra, RISCV64_KERNEL_CONTEXT_RA_OFFSET(a1)
    ld sp, RISCV64_KERNEL_CONTEXT_SP_OFFSET(a1)
    ld s0, RISCV64_KERNEL_CONTEXT_S0_OFFSET(a1)
    ld s1, RISCV64_KERNEL_CONTEXT_S1_OFFSET(a1)
    ld s2, RISCV64_KERNEL_CONTEXT_S2_OFFSET(a1)
    ld s3, RISCV64_KERNEL_CONTEXT_S3_OFFSET(a1)
    ld s4, RISCV64_KERNEL_CONTEXT_S4_OFFSET(a1)
    ld s5, RISCV64_KERNEL_CONTEXT_S5_OFFSET(a1)
    ld s6, RISCV64_KERNEL_CONTEXT_S6_OFFSET(a1)
    ld s7, RISCV64_KERNEL_CONTEXT_S7_OFFSET(a1)
    ld s8, RISCV64_KERNEL_CONTEXT_S8_OFFSET(a1)
    ld s9, RISCV64_KERNEL_CONTEXT_S9_OFFSET(a1)
    ld s10, RISCV64_KERNEL_CONTEXT_S10_OFFSET(a1)
    ld s11, RISCV64_KERNEL_CONTEXT_S11_OFFSET(a1)
    ret
.size arch_riscv64_switch_context, .-arch_riscv64_switch_context

.global arch_riscv64_enter_context
.hidden arch_riscv64_enter_context
.type arch_riscv64_enter_context, @function
arch_riscv64_enter_context:
    ld ra, RISCV64_KERNEL_CONTEXT_RA_OFFSET(a0)
    ld sp, RISCV64_KERNEL_CONTEXT_SP_OFFSET(a0)
    ld s0, RISCV64_KERNEL_CONTEXT_S0_OFFSET(a0)
    ld s1, RISCV64_KERNEL_CONTEXT_S1_OFFSET(a0)
    ld s2, RISCV64_KERNEL_CONTEXT_S2_OFFSET(a0)
    ld s3, RISCV64_KERNEL_CONTEXT_S3_OFFSET(a0)
    ld s4, RISCV64_KERNEL_CONTEXT_S4_OFFSET(a0)
    ld s5, RISCV64_KERNEL_CONTEXT_S5_OFFSET(a0)
    ld s6, RISCV64_KERNEL_CONTEXT_S6_OFFSET(a0)
    ld s7, RISCV64_KERNEL_CONTEXT_S7_OFFSET(a0)
    ld s8, RISCV64_KERNEL_CONTEXT_S8_OFFSET(a0)
    ld s9, RISCV64_KERNEL_CONTEXT_S9_OFFSET(a0)
    ld s10, RISCV64_KERNEL_CONTEXT_S10_OFFSET(a0)
    ld s11, RISCV64_KERNEL_CONTEXT_S11_OFFSET(a0)
    jr ra
.size arch_riscv64_enter_context, .-arch_riscv64_enter_context

.global arch_riscv64_context_start
.hidden arch_riscv64_context_start
.type arch_riscv64_context_start, @function
arch_riscv64_context_start:
    mv a0, s1
    jr s0
.size arch_riscv64_context_start, .-arch_riscv64_context_start
