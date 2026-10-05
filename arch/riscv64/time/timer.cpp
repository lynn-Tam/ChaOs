#include <expected>
#include <arch/time.hpp>

#include "arch/riscv64/cpu/csr.hpp"
#include <sbi.hpp>

namespace arch {

auto timer_available() noexcept -> bool { return sbi::probe(sbi::Ext::Timer); }

auto program_timer(time::Instant deadline) noexcept -> std::expected<void, TimerError> {
    const auto result = sbi::call(sbi::Ext::Timer, 0, deadline.ticks());
    if (result) {
        riscv64::Sie::enable_timer();
        return {};
    }
    if (result.error() == sbi::Unsupported) {
        return std::unexpected(TimerError::NotSupported);
    }
    return std::unexpected(TimerError::Rejected);
}

void mask_timer() noexcept { riscv64::Sie::disable_timer(); }

} // namespace arch
