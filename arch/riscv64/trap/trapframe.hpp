//arch/riscv64/trap/trapframe.hpp

#pragma once
#if !defined(__ASSEMBLER__)
#include <stddef.h>
#include <stdint.h>
#endif

#if !defined(__ASSEMBLER__)
#define RISCV64_TRAP_WORD_BYTES 8UL
// WHY: x0/zero 恒为 0，不需要保存。
#define RISCV64_TRAP_GPR_COUNT 31UL
#define RISCV64_TRAP_CSR_COUNT 4UL
#else
#define RISCV64_TRAP_WORD_BYTES 8
#define RISCV64_TRAP_GPR_COUNT 31
#define RISCV64_TRAP_CSR_COUNT 4
#endif

#define RISCV64_TRAP_SAVED_WORD_COUNT (RISCV64_TRAP_GPR_COUNT + RISCV64_TRAP_CSR_COUNT)

// WHY: 保存区是 35 words；额外 1 word 只为保持 sp 16-byte aligned。这样 trap entry 后续 call C++
// handler 时不破坏 RISC-V ABI。
#define RISCV64_TRAP_PADDING_WORD_COUNT 1
#define RISCV64_TRAP_FRAME_WORD_COUNT                                                              \
    (RISCV64_TRAP_SAVED_WORD_COUNT + RISCV64_TRAP_PADDING_WORD_COUNT)
#define RISCV64_TRAP_FRAME_SIZE (RISCV64_TRAP_FRAME_WORD_COUNT * RISCV64_TRAP_WORD_BYTES)

// GPR BYTE OFFSET.
#define RA_OFFSET 0
#define SP_OFFSET 8
#define GP_OFFSET 16
#define TP_OFFSET 24
#define T0_OFFSET 32
#define T1_OFFSET 40
#define T2_OFFSET 48
#define S0_OFFSET 56
#define S1_OFFSET 64
#define A0_OFFSET 72
#define A1_OFFSET 80
#define A2_OFFSET 88
#define A3_OFFSET 96
#define A4_OFFSET 104
#define A5_OFFSET 112
#define A6_OFFSET 120
#define A7_OFFSET 128
#define S2_OFFSET 136
#define S3_OFFSET 144
#define S4_OFFSET 152
#define S5_OFFSET 160
#define S6_OFFSET 168
#define S7_OFFSET 176
#define S8_OFFSET 184
#define S9_OFFSET 192
#define S10_OFFSET 200
#define S11_OFFSET 208
#define T3_OFFSET 216
#define T4_OFFSET 224
#define T5_OFFSET 232
#define T6_OFFSET 240

// csr byte offsets saved by the trap entry before calling c++.
#define SEPC_OFFSET 248
#define SSTATUS_OFFSET 256
#define SCAUSE_OFFSET 264
#define STVAL_OFFSET 272
#define PADDING_OFFSET 280

#if !defined(__ASSEMBLER__)
namespace arch::riscv64 {

struct TrapFrame {
    uint64_t ra;
    uint64_t sp;
    uint64_t gp;
    uint64_t tp;
    uint64_t t0;
    uint64_t t1;
    uint64_t t2;
    uint64_t s0;
    uint64_t s1;
    uint64_t a0;
    uint64_t a1;
    uint64_t a2;
    uint64_t a3;
    uint64_t a4;
    uint64_t a5;
    uint64_t a6;
    uint64_t a7;
    uint64_t s2;
    uint64_t s3;
    uint64_t s4;
    uint64_t s5;
    uint64_t s6;
    uint64_t s7;
    uint64_t s8;
    uint64_t s9;
    uint64_t s10;
    uint64_t s11;
    uint64_t t3;
    uint64_t t4;
    uint64_t t5;
    uint64_t t6;
    uint64_t sepc;
    uint64_t sstatus;
    uint64_t scause;
    uint64_t stval;
    uint64_t padding;
};

static_assert(sizeof(TrapFrame) == RISCV64_TRAP_FRAME_SIZE);
static_assert(alignof(TrapFrame) == RISCV64_TRAP_WORD_BYTES);
static_assert(offsetof(TrapFrame, ra) == RA_OFFSET);
static_assert(offsetof(TrapFrame, t6) == T6_OFFSET);
static_assert(offsetof(TrapFrame, sepc) == SEPC_OFFSET);
static_assert(offsetof(TrapFrame, sstatus) == SSTATUS_OFFSET);
static_assert(offsetof(TrapFrame, scause) == SCAUSE_OFFSET);
static_assert(offsetof(TrapFrame, stval) == STVAL_OFFSET);
static_assert(offsetof(TrapFrame, padding) == PADDING_OFFSET);

} // namespace arch::riscv64
#endif