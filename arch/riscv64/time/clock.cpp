#include <arch/time.hpp>

namespace arch {

auto read_clock() noexcept -> time::Instant {
    u64 ticks;
    asm volatile("rdtime %0" : "=r"(ticks));
    return time::Instant::from_ticks(ticks);
}

} // namespace arch
