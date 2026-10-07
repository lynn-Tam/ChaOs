#pragma once

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
#include <array>
#include <base/types.hpp>
#include <cpu.hpp>
#include <cstddef>
#include <libk/assert.hpp>
#include <mm/types.hpp>
#include <optional>

namespace arch {
// gpr[x-1] stores x1..x31; x0 is constant. Assembly uses the offsets above.
struct TrapFrame {
    std::array<usize, 31> gpr;
    usize sepc, sstatus, scause, stval, padding;
};
static_assert(sizeof(TrapFrame) == RISCV64_TRAP_FRAME_SIZE);
static_assert(alignof(TrapFrame) == RISCV64_TRAP_WORD_BYTES);
static_assert(offsetof(TrapFrame, gpr) == RA_OFFSET);
static_assert(sizeof(TrapFrame::gpr) == T6_OFFSET + sizeof(usize));
static_assert(offsetof(TrapFrame, sepc) == SEPC_OFFSET);
static_assert(offsetof(TrapFrame, sstatus) == SSTATUS_OFFSET);
static_assert(offsetof(TrapFrame, scause) == SCAUSE_OFFSET);
static_assert(offsetof(TrapFrame, stval) == STVAL_OFFSET);
static_assert(offsetof(TrapFrame, padding) == PADDING_OFFSET);

struct TrapRegs {
    std::array<usize, 31> gpr{};
    usize pc{}, status{}, cause{}, fault_address{};
    StackRegs stack() const noexcept { return {pc, gpr[1], gpr[7], gpr[0]}; }
};
struct UserStart {
    mm::Virt entry{}, stack{};
    std::array<usize, 6> arguments{};
};

// A typed borrow of the sole return frame; redirection selects another owned stack.
class TrapCtx {
  public:
    explicit TrapCtx(TrapFrame& f) noexcept : frame_(&f) {}
    TrapCtx(const TrapCtx&) = delete;
    auto operator=(const TrapCtx&) -> TrapCtx& = delete;
    usize pc() const noexcept { return frame_->sepc; }
    void set_pc(usize pc) noexcept { frame_->sepc = pc; }
    void complete_breakpoint() noexcept;
    void complete_syscall() noexcept { frame_->sepc += 4; }
    usize arg(usize i) const noexcept { libk_assert(i < 8); return frame_->gpr[9 + i]; }
    void set_result(usize i, usize value) noexcept { libk_assert(i < 3); frame_->gpr[9 + i] = value; }
    void set_return(usize value) noexcept { set_result(0, value); }
    usize fault_addr() const noexcept { return frame_->stval; }
    TrapRegs snapshot() const noexcept {
        return {frame_->gpr, frame_->sepc, frame_->sstatus, frame_->scause, frame_->stval};
    }
    bool load_user_start(const UserStart&) noexcept;
    TrapFrame* frame() const noexcept { return frame_; }
    void redirect(TrapFrame* frame) noexcept { libk_assert(frame); frame_ = frame; }
  private:
    TrapFrame* frame_;
};

bool install_trap() noexcept;
bool valid_user_start(UserStart) noexcept;
std::optional<TrapFrame*> prepare_user_frame(usize top, UserStart) noexcept;
std::optional<usize> prepare_user_stack(usize top, UserStart) noexcept;
[[noreturn]] void resume_user(usize top) noexcept;
} // namespace arch
#endif
