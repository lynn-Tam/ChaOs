#include "sbi.hpp"
#include <cpu.hpp>

namespace sbi {
auto call(Ext ext, usize fn, usize x, usize y, usize z) noexcept -> std::expected<usize, isize> {
    register isize a0 asm("a0") = static_cast<isize>(x);
    register usize a1 asm("a1") = y;
    register usize a2 asm("a2") = z;
    register usize a6 asm("a6") = fn;
    register usize a7 asm("a7") = static_cast<usize>(ext);
    asm volatile("ecall" : "+r"(a0), "+r"(a1) : "r"(a2), "r"(a6), "r"(a7) : "memory");
    if (a0) return std::unexpected(static_cast<isize>(a0));
    return static_cast<usize>(a1);
}
} // namespace sbi

namespace arch {
[[noreturn]] void halt_system(HaltAction action, HaltReason reason) noexcept {
    static_cast<void>(disable_interrupts());
    static_cast<void>(sbi::call(sbi::Ext::Reset, 0, action == HaltAction::Shutdown ? 0 : 1,
                                reason == HaltReason::PeerStop ? 0 : 1));
    halt_current_cpu(reason);
}
void putchar(char c) noexcept {
    const auto byte = static_cast<unsigned char>(c);
    if (!sbi::call(sbi::Ext::Console, 2, byte)) {
        // SBI 0.1 remains the early-console fallback for older firmware.
        static_cast<void>(sbi::call(sbi::Ext::PutChar, 0, byte));
    }
}
} // namespace arch
