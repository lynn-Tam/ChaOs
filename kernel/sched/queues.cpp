#include <optional>
#include <sched/queues.hpp>

#include <bit>

namespace sched {

void ReadyQueue::enqueue(Sc& sc, Urgency urgency) noexcept {
    libk_assert(!sc.queued());
    const u8 level = urgency.value();
    levels_[level].push_back(sc);
    bitmap_ |= u32{1} << level;
    ++size_;
}

void ReadyQueue::remove(Sc& sc, Urgency urgency) noexcept {
    libk_assert(sc.queued());
    const u8 level = urgency.value();
    auto& queue = levels_[level];
    queue.erase(sc);
    libk_assert(size_ != 0);
    --size_;
    if (queue.empty()) {
        bitmap_ &= ~(u32{1} << level);
    }
}

auto ReadyQueue::front() noexcept -> Sc* {
    if (!bitmap_) return nullptr;
    const usize level = 31 - std::countl_zero(bitmap_);
    return &levels_[level].front();
}

auto ReadyQueue::select() noexcept -> Sc* { return front(); }

auto ReadyQueue::pop_front(Urgency urgency) noexcept -> Sc* {
    const u8 level = urgency.value();
    auto& queue = levels_[level];
    if (queue.empty()) {
        return nullptr;
    }
    Sc& sc = queue.pop_front();
    libk_assert(size_ != 0);
    --size_;
    if (queue.empty()) {
        bitmap_ &= ~(u32{1} << level);
    }
    return &sc;
}

Deadline::~Deadline() noexcept {
    libk_assert(!armed() && !hook_.is_linked());
    libk_assert(callback_);
}

auto DeadlineQueue::deadline() const noexcept
    -> std::optional<time::Instant> {
    const Deadline* const deadline = tree_.minimum();
    return deadline != nullptr
        ? std::optional<time::Instant>{deadline->when_}
        : std::nullopt;
}

void DeadlineQueue::insert(
    Deadline& deadline,
    time::Instant when) noexcept {
    libk_assert(!deadline.armed() && !deadline.hook_.is_linked());
    deadline.when_ = when;
    tree_.insert(deadline);
}

void DeadlineQueue::remove(Deadline& deadline) noexcept {
    libk_assert(deadline.hook_.is_linked());
    tree_.erase(deadline);
}

auto TimerQueue::deadline() const noexcept -> std::optional<time::Instant> {
    const Sc* const sc = tree_.minimum();
    if (sc == nullptr) {
        return std::nullopt;
    }
    return sc->timer_deadline_;
}

void TimerQueue::insert(
    Sc& sc,
    time::Instant deadline) noexcept {
    libk_assert(!sc.timer_queued());
    sc.timer_deadline_ = deadline;
    tree_.insert(sc);
}

void TimerQueue::remove(Sc& sc) noexcept {
    libk_assert(sc.timer_queued());
    tree_.erase(sc);
}

} // namespace sched
