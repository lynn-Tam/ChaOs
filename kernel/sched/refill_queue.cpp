#include <sched/refill_queue.hpp>

#include <core/debug.hpp>
#include <libk/checked_arithmetic.hpp>

namespace kernel::sched {

RefillQueue::RefillQueue(
    time::Duration budget,
    time::Duration period,
    usize capacity,
    time::Instant now) noexcept
    : budget_(budget), period_(period), capacity_(capacity) {
    KASSERT(!budget_.empty() && budget_ <= period_);
    KASSERT(capacity_ != 0 && capacity_ <= max_capacity);
    KASSERT(entries_.try_emplace_back(Refill{now, budget_}) != nullptr);
}

auto RefillQueue::available(time::Instant now) const noexcept
    -> time::Duration {
    u64 total{};
    for (const Refill& refill : entries_) {
        if (refill.ready_at > now) {
            break;
        }
        const auto sum = libk::checked_add(total, refill.amount.ticks());
        KASSERT(sum);
        total = *sum;
    }
    KASSERT(total <= budget_.ticks());
    return time::Duration::from_ticks(total);
}

auto RefillQueue::next() const noexcept -> libk::optional<time::Instant> {
    return entries_.empty()
        ? libk::optional<time::Instant>{libk::nullopt}
        : libk::optional<time::Instant>{entries_.front().ready_at};
}

void RefillQueue::append(Refill refill) noexcept {
    KASSERT(!refill.amount.empty());
    if (!entries_.empty() && entries_.back().ready_at == refill.ready_at) {
        const auto sum = libk::checked_add(
            entries_.back().amount.ticks(), refill.amount.ticks());
        KASSERT(sum && *sum <= budget_.ticks());
        entries_.back().amount = time::Duration::from_ticks(*sum);
        return;
    }
    if (entries_.size() >= capacity_) {
        Refill& last = entries_.back();
        const auto sum = libk::checked_add(
            last.amount.ticks(), refill.amount.ticks());
        KASSERT(sum && *sum <= budget_.ticks());
        KASSERT(last.ready_at <= refill.ready_at);
        last.ready_at = refill.ready_at;
        last.amount = time::Duration::from_ticks(*sum);
        return;
    }
    KASSERT(entries_.try_emplace_back(refill) != nullptr);
}

auto RefillQueue::charge(
    time::Instant now,
    time::Duration elapsed) noexcept -> time::Duration {
    u64 remaining = elapsed.ticks();
    u64 consumed{};
    while (remaining != 0 && !entries_.empty()) {
        Refill& refill = entries_.front();
        if (refill.ready_at > now) {
            break;
        }
        const u64 amount = refill.amount.ticks();
        const u64 taken = amount < remaining ? amount : remaining;
        remaining -= taken;
        consumed += taken;
        if (taken == amount) {
            entries_.pop_front();
        } else {
            refill.amount = time::Duration::from_ticks(amount - taken);
        }
    }
    if (consumed != 0) {
        const auto ready = now.checked_add(period_);
        KASSERT(ready);
        append(Refill{*ready, time::Duration::from_ticks(consumed)});
    }
    verify_conservation();
    return time::Duration::from_ticks(remaining);
}

void RefillQueue::verify_conservation() const noexcept {
    u64 total{};
    for (const Refill& refill : entries_) {
        const auto sum = libk::checked_add(total, refill.amount.ticks());
        KASSERT(sum);
        total = *sum;
    }
    KASSERT(total == budget_.ticks());
}

} // namespace kernel::sched
