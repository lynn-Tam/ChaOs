#include <optional>
#include <arch/user.hpp>
#include <arch/trap.hpp>

#include "arch/riscv64/cpu/csr.hpp"
#include "arch/riscv64/trap/context.hpp"
#include "arch/riscv64/trap/trapframe.hpp"

#include <mm/table.hpp>
#include <libk/assert.hpp>
#include <base/types.hpp>
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
    return mm::is_user(start.entry)
        && (start.entry.raw() & 0x1U) == 0
        && start.stack.raw() >= mm::UserBegin
        && start.stack.raw() <= mm::UserEnd
        && (start.stack.raw() & 0xfU) == 0;
}

auto prepare_user_stack(
    usize home_stack_top,
    UserStart start) noexcept -> std::optional<usize> {
    if (!valid_user_start(start)
        || home_stack_top < sizeof(riscv64::TrapFrame)
        || (home_stack_top & 0xfU) != 0) {
        return std::nullopt;
    }

    riscv64::TrapFrame* const frame = frame_at(home_stack_top);
    libk_assert((reinterpret_cast<usize>(frame) & 0xfU) == 0);
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

auto prepare_user_frame(
    usize kernel_stack_top,
    UserStart start) noexcept -> std::optional<UserFrame> {
    const auto frame = prepare_user_stack(kernel_stack_top, start);
    return frame
        ? std::optional<UserFrame>{
              riscv64::make_user_frame(
                  *reinterpret_cast<riscv64::TrapFrame*>(*frame))}
        : std::nullopt;
}

[[noreturn]] void resume_user(usize home_stack_top) noexcept {
    riscv64::TrapFrame* const frame = frame_at(home_stack_top);
    libk_assert(mm::is_user(mm::Virt{frame->sepc}));
    libk_assert(frame->sstatus == riscv64::Sstatus::SPIE);
    libk_assert((frame->sp & 0xfU) == 0);
    arch_riscv64_resume_user(frame);
}

} // namespace arch
