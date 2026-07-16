// arch/riscv64/trap/context.cpp
// 实现 selected-arch TrapContext view 对 RISC-V TrapFrame 的受控访问。

#include "arch/riscv64/trap/context.hpp"

#include <core/debug.hpp>

namespace arch {

namespace {

constexpr usize ebreak_size = 4;
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
};

TrapContext::TrapContext(void* frame) noexcept
    : frame_(frame) {
    KASSERT(frame_ != nullptr);
}

namespace riscv64 {

auto make_context(TrapFrame& frame) noexcept -> arch::TrapContext {
    return TrapContextAccess::from_raw(frame);
}

} // namespace riscv64

auto TrapContext::pc() const noexcept -> usize {
    return frame_of(frame_).sepc;
}

void TrapContext::set_pc(usize pc) noexcept {
    frame_of(frame_).sepc = pc;
}

void TrapContext::complete_breakpoint() noexcept {
    // RISC-V leaves sepc on ebreak; compressed c.ebreak needs width decode later.
    frame_of(frame_).sepc += ebreak_size;
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
        KASSERT(false);
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
    default:
        KASSERT(false);
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

} // namespace arch
