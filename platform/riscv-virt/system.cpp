#include <platform/system.hpp>

#include "arch/riscv64/sbi/call.hpp"

#include <arch/cpu.hpp>
#include <arch/interrupt.hpp>

namespace platform {

[[noreturn]] void halt_current_cpu([[maybe_unused]] HaltReason reason) noexcept {
    static_cast<void>(arch::disable_interrupts());
    for (;;) {
        arch::wait_for_interrupt();
    }
}

[[noreturn]] void halt_system(
    HaltAction action,
    HaltReason reason) noexcept {
    constexpr usize system_reset_extension = 0x53525354;
    constexpr usize reset_function = 0;
    constexpr usize shutdown = 0;
    constexpr usize cold_reboot = 1;
    constexpr usize no_reason = 0;
    constexpr usize system_failure = 1;

    static_cast<void>(arch::disable_interrupts());
    const usize reset_type = action == HaltAction::Shutdown
        ? shutdown
        : cold_reboot;
    const usize reset_reason = reason == HaltReason::PeerStop
        ? no_reason
        : system_failure;
    static_cast<void>(arch::riscv64::sbi::call(
        system_reset_extension,
        reset_function,
        reset_type,
        reset_reason));
    halt_current_cpu(reason);
}

} // namespace platform
