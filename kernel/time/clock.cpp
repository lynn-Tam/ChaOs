#include <time/clock.hpp>

#include <arch/time.hpp>
#include <core/debug.hpp>

namespace kernel::time {

auto Clock::now() const noexcept -> Instant {
    KASSERT(valid());
    return arch::read_clock();
}

} // namespace kernel::time
