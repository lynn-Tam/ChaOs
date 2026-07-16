#include <arch/system.hpp>

#include <arch/cpu.hpp>
#include <arch/interrupt.hpp>

#include "call.hpp"

namespace arch {

[[noreturn]] void halt_current_cpu([[maybe_unused]] HaltReason reason) noexcept {
    static_cast<void>(disable_interrupts());
    for (;;) {
        wait_for_interrupt();
    }
}

[[noreturn]] void halt_system(HaltAction action, HaltReason reason) noexcept {
    constexpr usize SystemResetExtension = 0x53525354;
    constexpr usize ResetFunction = 0;
    constexpr usize Shutdown = 0;
    constexpr usize ColdReboot = 1;
    constexpr usize NoReason = 0;
    constexpr usize SystemFailure = 1;

    static_cast<void>(disable_interrupts());
    const usize reset_type = action == HaltAction::Shutdown
        ? Shutdown
        : ColdReboot;
    const usize reset_reason = reason == HaltReason::PeerStop
        ? NoReason
        : SystemFailure;
    static_cast<void>(riscv64::sbi::call(
        SystemResetExtension,
        ResetFunction,
        reset_type,
        reset_reason));
    halt_current_cpu(reason);
}

} // namespace arch
