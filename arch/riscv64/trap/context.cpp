// arch/riscv64/trap/context.cpp
// 实现 selected-arch TrapContext view 对 RISC-V TrapFrame 的受控访问。

#include "arch/riscv64/trap/context.hpp"
#include "arch/riscv64/cpu/csr.hpp"

#include <arch/user.hpp>
#include <core/debug.hpp>
#include <libk/memory.hpp>

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

    [[nodiscard]] static auto frame(void* raw) noexcept -> UserFrame {
        return UserFrame{raw};
    }

    [[nodiscard]] static auto raw(UserFrame frame) noexcept -> void* {
        return frame.raw_;
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
    case 2:
        frame.a2 = value;
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

void TrapContext::save_user(myos_user_context& output) const noexcept {
    const auto& frame = frame_of(frame_);
    output = {};
    output.words[0] = frame.sepc;
    const auto* const registers = &frame.ra;
    for (usize index = 0; index < 31; ++index) {
        output.words[index + 1] = registers[index];
    }
}

auto TrapContext::load_user(const myos_user_context& input) noexcept -> bool {
    if (!valid_user_context(input)) {
        return false;
    }
    auto& frame = frame_of(frame_);
    auto* const registers = &frame.ra;
    for (usize index = 0; index < 31; ++index) {
        registers[index] = input.words[index + 1];
    }
    frame.sepc = input.words[0];
    // User memory never contributes supervisor-controlled status.
    frame.sstatus = riscv64::Sstatus::SPIE;
    frame.scause = 0;
    frame.stval = 0;
    frame.padding = 0;
    return true;
}

auto TrapContext::load_user_start(const UserStart& start) noexcept -> bool {
    if (!valid_user_start(start)) {
        return false;
    }
    myos_user_context context{};
    context.words[0] = start.entry.raw();
    context.words[2] = start.stack.raw();
    constexpr usize A0 = 10;
    for (usize index = 0; index < 6; ++index) {
        context.words[A0 + index] = start.arguments[index];
    }
    return load_user(context);
}

auto TrapContext::frame() const noexcept -> UserFrame {
    return TrapContextAccess::frame(frame_);
}

void TrapContext::redirect(UserFrame frame) noexcept {
    KASSERT(frame);
    frame_ = TrapContextAccess::raw(frame);
}

} // namespace arch
