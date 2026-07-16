#include <arch/user.hpp>

#include "arch/riscv64/cpu/csr.hpp"
#include "arch/riscv64/trap/trapframe.hpp"

#include <mm/virtual_layout.hpp>
#include <core/debug.hpp>
#include <libk/memory.hpp>

extern "C" [[noreturn]] void arch_riscv64_resume_user(
    arch::riscv64::TrapFrame* frame) noexcept;

namespace arch {
namespace {

[[nodiscard]] auto frame_at(usize home_stack_top) noexcept
    -> riscv64::TrapFrame* {
    return reinterpret_cast<riscv64::TrapFrame*>(
        home_stack_top - sizeof(riscv64::TrapFrame));
}

} // namespace

auto valid_user_start(UserStart start) noexcept -> bool {
    return kernel::mm::layout::is_user(start.entry)
        && (start.entry.raw() & 0x1U) == 0
        && start.stack.raw() >= kernel::mm::layout::LowGuardEnd
        && start.stack.raw() <= kernel::mm::layout::UserEnd
        && (start.stack.raw() & 0xfU) == 0;
}

auto prepare_user_stack(
    usize home_stack_top,
    UserStart start) noexcept -> libk::optional<usize> {
    if (!valid_user_start(start)
        || home_stack_top < sizeof(riscv64::TrapFrame)
        || (home_stack_top & 0xfU) != 0) {
        return libk::nullopt;
    }

    riscv64::TrapFrame* const frame = frame_at(home_stack_top);
    KASSERT((reinterpret_cast<usize>(frame) & 0xfU) == 0);
    libk::construct_at(frame);
    *frame = {};
    frame->sp = start.stack.raw();
    frame->a0 = start.arguments[0];
    frame->a1 = start.arguments[1];
    frame->a2 = start.arguments[2];
    frame->a3 = start.arguments[3];
    frame->a4 = start.arguments[4];
    frame->a5 = start.arguments[5];
    frame->sepc = start.entry.raw();
    // SPP=U, SIE=0, SPIE=1. SUM/MXR and every other supervisor-controlled
    // field remain clear; user input never contributes raw status bits.
    frame->sstatus = riscv64::Sstatus::SPIE;
    return reinterpret_cast<usize>(frame);
}

[[noreturn]] void resume_user(usize home_stack_top) noexcept {
    riscv64::TrapFrame* const frame = frame_at(home_stack_top);
    KASSERT(kernel::mm::layout::is_user(kernel::mm::VirtAddr{frame->sepc}));
    KASSERT(frame->sstatus == riscv64::Sstatus::SPIE);
    KASSERT((frame->sp & 0xfU) == 0);
    arch_riscv64_resume_user(frame);
}

} // namespace arch
