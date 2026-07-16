#include <sched/timer_queue.hpp>

#include <core/debug.hpp>

namespace kernel::sched {

auto TimerQueue::deadline() const noexcept -> libk::optional<time::Instant> {
    const Binding* const binding = tree_.minimum();
    if (binding == nullptr) {
        return libk::nullopt;
    }
    return binding->timer_deadline_;
}

void TimerQueue::insert(
    Binding& binding,
    time::Instant deadline) noexcept {
    KASSERT(!binding.timer_queued());
    binding.timer_deadline_ = deadline;
    tree_.insert(binding);
}

void TimerQueue::remove(Binding& binding) noexcept {
    KASSERT(binding.timer_queued());
    tree_.erase(binding);
}

} // namespace kernel::sched
