#include "cpu_abi.h"

/* SBI enters here in Bare mode with a physical boot-record pointer in a1. */
.section .boot.secondary, "ax", @progbits
.balign 4

.global secondary_entry
.hidden secondary_entry
.type secondary_entry, @function
secondary_entry:
    csrci sstatus, 2

    mv s0, a0
    mv s1, a1
    beqz s1, .Lsecondary_park
    andi t0, s1, 7
    bnez t0, .Lsecondary_park

    lw t0, CPU_START_READY_OFF(s1)
    fence r, rw
    li t1, CPU_START_READY
    bne t0, t1, .Lsecondary_park

    ld t0, CPU_START_HART_OFF(s1)
    bne t0, s0, .Lsecondary_park

    /* No physical pointer is read after SATP changes. */
    ld s2, CPU_START_ROOT_OFF(s1)
    ld s3, CPU_START_STACK_OFF(s1)
    ld s4, CPU_START_RUNTIME_OFF(s1)
    ld s5, CPU_START_ENTRY_OFF(s1)
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

.size secondary_entry, .-secondary_entry

.section .text, "ax", @progbits
.balign 4
.global switch_stack
.hidden switch_stack
.type switch_stack, @function
switch_stack:
    mv sp, a0
    mv a0, a1
    jr a2
.size switch_stack, .-switch_stack

/* One restore path serves first entry and a returning context switch. */
.balign 4
.global switch_ctx
.hidden switch_ctx
.type switch_ctx, @function
switch_ctx:
    sd ra, CPU_CTX_RA_OFF(a0)
    sd sp, CPU_CTX_SP_OFF(a0)
    sd s0, CPU_CTX_S_OFF+0(a0)
    sd s1, CPU_CTX_S_OFF+8(a0)
    sd s2, CPU_CTX_S_OFF+16(a0)
    sd s3, CPU_CTX_S_OFF+24(a0)
    sd s4, CPU_CTX_S_OFF+32(a0)
    sd s5, CPU_CTX_S_OFF+40(a0)
    sd s6, CPU_CTX_S_OFF+48(a0)
    sd s7, CPU_CTX_S_OFF+56(a0)
    sd s8, CPU_CTX_S_OFF+64(a0)
    sd s9, CPU_CTX_S_OFF+72(a0)
    sd s10, CPU_CTX_S_OFF+80(a0)
    sd s11, CPU_CTX_S_OFF+88(a0)
    mv a0, a1
.size switch_ctx, .-switch_ctx

.global enter_ctx
.hidden enter_ctx
.type enter_ctx, @function
enter_ctx:
    ld ra, CPU_CTX_RA_OFF(a0)
    ld sp, CPU_CTX_SP_OFF(a0)
    ld s0, CPU_CTX_S_OFF+0(a0)
    ld s1, CPU_CTX_S_OFF+8(a0)
    ld s2, CPU_CTX_S_OFF+16(a0)
    ld s3, CPU_CTX_S_OFF+24(a0)
    ld s4, CPU_CTX_S_OFF+32(a0)
    ld s5, CPU_CTX_S_OFF+40(a0)
    ld s6, CPU_CTX_S_OFF+48(a0)
    ld s7, CPU_CTX_S_OFF+56(a0)
    ld s8, CPU_CTX_S_OFF+64(a0)
    ld s9, CPU_CTX_S_OFF+72(a0)
    ld s10, CPU_CTX_S_OFF+80(a0)
    ld s11, CPU_CTX_S_OFF+88(a0)
    jr ra
.size enter_ctx, .-enter_ctx

.hidden ctx_entry
.global ctx_entry
.type ctx_entry, @function
ctx_entry:
    mv a0, s1
    jr s0
.size ctx_entry, .-ctx_entry
