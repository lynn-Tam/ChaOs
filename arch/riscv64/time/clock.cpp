#include <arch/time.hpp>

namespace arch {

auto read_clock() noexcept -> kernel::time::Instant {
    u64 ticks;
    asm volatile("rdtime %0" : "=r"(ticks));
    return kernel::time::Instant::from_ticks(ticks);
}

} // namespace arch
