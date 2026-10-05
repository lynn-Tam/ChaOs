// arch/riscv64/trap/context.cpp
// 实现 selected-arch TrapContext view 对 RISC-V TrapFrame 的受控访问。

#include "arch/riscv64/trap/context.hpp"
#include "arch/riscv64/cpu/csr.hpp"

#include <arch/instruction.hpp>
#include <arch/user.hpp>
#include <libk/assert.hpp>
#include <base/types.hpp>
#include <libk/mem.h>
#include <libk/memory.hpp>

namespace arch {

namespace {

constexpr usize ecall_size = 4;

[[nodiscard]] auto frame_of(void* frame) noexcept -> riscv64::TrapFrame& {
    return *static_cast<riscv64::TrapFrame*>(frame);
}

} // namespace

struct TrapContextAccess final {
    [[nodiscard]] static auto from_raw(riscv64::TrapFrame& frame) noexcept
        -> TrapContext {
        return TrapContext{&frame};
    }

    [[nodiscard]] static auto frame(void* raw) noexcept -> UserFrame {
        return UserFrame{raw};
    }

    [[nodiscard]] static auto raw(UserFrame frame) noexcept -> void* {
        return frame.raw_;
    }
};

TrapContext::TrapContext(void* frame) noexcept
    : frame_(frame) {
    libk_assert(frame_ != nullptr);
}

namespace riscv64 {

auto make_context(TrapFrame& frame) noexcept -> arch::TrapContext {
    return TrapContextAccess::from_raw(frame);
}

auto make_user_frame(TrapFrame& frame) noexcept -> arch::UserFrame {
    return TrapContextAccess::frame(&frame);
}

auto raw_frame(arch::UserFrame frame) noexcept -> TrapFrame* {
    return static_cast<TrapFrame*>(TrapContextAccess::raw(frame));
}

} // namespace riscv64

auto TrapContext::pc() const noexcept -> usize {
    return frame_of(frame_).sepc;
}

void TrapContext::set_pc(usize pc) noexcept {
    frame_of(frame_).sepc = pc;
}

void TrapContext::complete_breakpoint() noexcept {
    auto& frame = frame_of(frame_);
    // The common trap policy calls this only for a supervisor-origin trap, so
    // sepc names the mapped instruction the hart just executed. Copying the
    // first parcel avoids an aliasing load and handles both ebreak/c.ebreak.
    u16 first{};
    memcpy(&first, reinterpret_cast<const void*>(frame.sepc), sizeof(first));
    frame.sepc += instruction_size(first);
}

void TrapContext::complete_syscall() noexcept {
    frame_of(frame_).sepc += ecall_size;
}

auto TrapContext::arg(usize index) const noexcept -> usize {
    const auto& frame = frame_of(frame_);

    switch (index) {
    case 0:
        return frame.a0;
    case 1:
        return frame.a1;
    case 2:
        return frame.a2;
    case 3:
        return frame.a3;
    case 4:
        return frame.a4;
    case 5:
        return frame.a5;
    case 6:
        return frame.a6;
    case 7:
        return frame.a7;
    default:
        libk_assert(false);
        __builtin_unreachable();
    }
}

void TrapContext::set_result(usize index, usize value) noexcept {
    auto& frame = frame_of(frame_);
    switch (index) {
    case 0:
        frame.a0 = value;
        return;
    case 1:
        frame.a1 = value;
        return;
    case 2:
        frame.a2 = value;
        return;
    default:
        libk_assert(false);
    }
}

auto TrapContext::fault_addr() const noexcept -> usize {
    return frame_of(frame_).stval;
}

auto TrapContext::snapshot() const noexcept -> TrapSnapshot {
    const auto& frame = frame_of(frame_);
    TrapSnapshot result{};
    const auto* const registers = &frame.ra;
    for (usize index = 0; index < 31; ++index) {
        result.gpr[index] = registers[index];
    }
    result.pc = frame.sepc;
    result.status = frame.sstatus;
    result.cause = frame.scause;
    result.fault_address = frame.stval;
    return result;
}

auto TrapContext::load_user_start(const UserStart& start) noexcept -> bool {
    if (!valid_user_start(start)) return false;
    auto& f = frame_of(frame_);
    f = {};
    f.sepc = start.entry.raw();
    f.sp = start.stack.raw();
    f.a0 = start.arguments[0];
    f.a1 = start.arguments[1];
    f.a2 = start.arguments[2];
    f.a3 = start.arguments[3];
    f.a4 = start.arguments[4];
    f.a5 = start.arguments[5];
    f.sstatus = riscv64::Sstatus::SPIE;
    return true;
}

auto TrapContext::frame() const noexcept -> UserFrame {
    return TrapContextAccess::frame(frame_);
}

void TrapContext::redirect(UserFrame frame) noexcept {
    libk_assert(frame);
    frame_ = TrapContextAccess::raw(frame);
}

} // namespace arch
