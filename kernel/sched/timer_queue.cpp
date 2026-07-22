#include <sched/timer_queue.hpp>

#include <core/debug.hpp>

namespace kernel::sched {

Deadline::~Deadline() noexcept {
    KASSERT(!armed() && !hook_.is_linked());
    KASSERT(callback_);
}

auto DeadlineQueue::deadline() const noexcept
    -> libk::optional<time::Instant> {
    const Deadline* const deadline = tree_.minimum();
    return deadline != nullptr
        ? libk::optional<time::Instant>{deadline->when_}
        : libk::nullopt;
}

void DeadlineQueue::insert(
    Deadline& deadline,
    time::Instant when) noexcept {
    KASSERT(!deadline.armed() && !deadline.hook_.is_linked());
    deadline.when_ = when;
    tree_.insert(deadline);
}

void DeadlineQueue::remove(Deadline& deadline) noexcept {
    KASSERT(deadline.hook_.is_linked());
    tree_.erase(deadline);
}

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
