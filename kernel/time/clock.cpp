#include <time/clock.hpp>

#include <arch/time.hpp>
#include <libk/assert.hpp>

namespace time {

auto Clock::now() const noexcept -> Instant {
    libk_assert(valid());
    return arch::read_clock();
}

} // namespace time
