#pragma once

#include <core/types.hpp>
#include <libk/intrusive_tree.hpp>
#include <sched/binding.hpp>
#include <time/time.hpp>

namespace kernel::sched {

// CpuDispatcher-owned ordered index of throttled Bindings.
// Only the owning CPU mutates it with interrupts disabled. Ordering is by the
// SC's next refill deadline, then stable Binding address. Refill amounts remain
// canonical in SchedulingContext; this queue stores no budget copy.
class TimerQueue final {
    struct Earlier final {
        [[nodiscard]] auto operator()(
            const Binding& lhs,
            const Binding& rhs) const noexcept -> bool {
            if (lhs.timer_deadline_ != rhs.timer_deadline_) {
                return lhs.timer_deadline_ < rhs.timer_deadline_;
            }
            return reinterpret_cast<usize>(&lhs)
                < reinterpret_cast<usize>(&rhs);
        }
    };
    using Tree = libk::IntrusiveTree<
        Binding, &Binding::timer_hook_, Earlier>;

public:
    [[nodiscard]] auto empty() const noexcept -> bool { return tree_.empty(); }
    [[nodiscard]] auto size() const noexcept -> usize { return tree_.size(); }
    [[nodiscard]] auto deadline() const noexcept
        -> libk::optional<time::Instant>;
    [[nodiscard]] auto front() noexcept -> Binding* { return tree_.minimum(); }

    void insert(Binding& binding, time::Instant deadline) noexcept;
    void remove(Binding& binding) noexcept;

private:
    Tree tree_{};
};

} // namespace kernel::sched
