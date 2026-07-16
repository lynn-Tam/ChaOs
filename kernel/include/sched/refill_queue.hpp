#pragma once

#include <core/types.hpp>
#include <libk/inplace_ring.hpp>
#include <libk/noncopyable.hpp>
#include <libk/optional.hpp>
#include <time/time.hpp>

namespace kernel::sched {

struct Refill final {
    time::Instant ready_at{};
    time::Duration amount{};
};

// SchedulingContext-owned canonical CPU-time ledger. RefillQueue is ordered
// by ready_at and conserves exactly Q ticks. Capacity pressure may delay an
// older refill by merging it into a later one, but can never advance budget.
class RefillQueue final : private libk::noncopyable_nonmovable {
public:
    static constexpr usize max_capacity = 8;

    RefillQueue(
        time::Duration budget,
        time::Duration period,
        usize capacity,
        time::Instant now) noexcept;

    [[nodiscard]] auto available(time::Instant now) const noexcept
        -> time::Duration;
    [[nodiscard]] auto next() const noexcept
        -> libk::optional<time::Instant>;
    [[nodiscard]] auto charge(
        time::Instant now,
        time::Duration elapsed) noexcept -> time::Duration;
    [[nodiscard]] auto size() const noexcept -> usize {
        return entries_.size();
    }

private:
    void append(Refill refill) noexcept;
    void verify_conservation() const noexcept;

    time::Duration budget_{};
    time::Duration period_{};
    usize capacity_{};
    libk::InplaceRing<Refill, max_capacity> entries_{};
};

} // namespace kernel::sched
