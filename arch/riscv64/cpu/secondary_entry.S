#include "arch/riscv64/cpu/start_context.hpp"

/* SBI enters here in Bare mode with a physical boot-record pointer in a1. */
.section .boot.secondary, "ax", @progbits
.balign 4

.global arch_riscv64_secondary_entry
.hidden arch_riscv64_secondary_entry
.type arch_riscv64_secondary_entry, @function
arch_riscv64_secondary_entry:
    csrci sstatus, 2

    mv s0, a0
    mv s1, a1
    beqz s1, .Lsecondary_park
    andi t0, s1, 7
    bnez t0, .Lsecondary_park

    lw t0, RISCV64_CPU_START_PUBLICATION_OFFSET(s1)
    fence r, rw
    li t1, RISCV64_CPU_START_READY
    bne t0, t1, .Lsecondary_park

    ld t0, RISCV64_CPU_START_HARDWARE_ID_OFFSET(s1)
    bne t0, s0, .Lsecondary_park

    /* No physical pointer is read after SATP changes. */
    ld s2, RISCV64_CPU_START_SATP_OFFSET(s1)
    ld s3, RISCV64_CPU_START_STACK_TOP_OFFSET(s1)
    ld s4, RISCV64_CPU_START_RUNTIME_OFFSET(s1)
    ld s5, RISCV64_CPU_START_ENTRY_OFFSET(s1)
    beqz s2, .Lsecondary_park
    beqz s3, .Lsecondary_park
    beqz s4, .Lsecondary_park
    beqz s5, .Lsecondary_park
    andi t0, s3, 15
    bnez t0, .Lsecondary_park
    /* rv64gc has IALIGN=16; a high continuation may be 2-byte aligned. */
    andi t0, s5, 1
    bnez t0, .Lsecondary_park

    la t0, .Lsecondary_park
    csrw stvec, t0
    sfence.vma x0, x0
    csrw satp, s2
    sfence.vma x0, x0

    mv a0, s4
    mv a1, s0
    mv sp, s3
    jr s5

.balign 4
.Lsecondary_park:
    csrci sstatus, 2
1:
    wfi
    j 1b

.size arch_riscv64_secondary_entry, .-arch_riscv64_secondary_entry
