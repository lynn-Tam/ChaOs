#include <arch/time.hpp>

#include "arch/riscv64/cpu/csr.hpp"
#include "arch/riscv64/sbi/base.hpp"
#include "arch/riscv64/sbi/time.hpp"

namespace arch {

auto timer_available() noexcept -> bool {
    return riscv64::sbi::extension_available(
        riscv64::sbi::time_extension_id);
}

auto program_timer(kernel::time::Instant deadline) noexcept
    -> libk::Expected<void, TimerError> {
    const riscv64::sbi::Ret result =
        riscv64::sbi::set_timer(deadline.ticks());
    if (result.error == riscv64::sbi::success) {
        riscv64::Sie::enable_timer();
        return libk::expected();
    }
    if (result.error == riscv64::sbi::not_supported) {
        return libk::unexpected(TimerError::NotSupported);
    }
    return libk::unexpected(TimerError::Rejected);
}

void mask_timer() noexcept {
    riscv64::Sie::disable_timer();
}

} // namespace arch
