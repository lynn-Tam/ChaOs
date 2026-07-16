#include <sched/context.hpp>

#include <core/debug.hpp>
#include <libk/checked_arithmetic.hpp>
#include <thread/thread.hpp>

namespace kernel::sched {

SchedulingContext::SchedulingContext(Config config, time::Instant now) noexcept
    : config_(config),
      refills_(config.budget, config.period, config.refill_capacity, now) {
    KASSERT(valid_config(config_));
}

SchedulingContext::~SchedulingContext() noexcept {
    KASSERT(!active_cpu_);
    KASSERT(!binding_);
    KASSERT(domain_ == nullptr);
}

auto SchedulingContext::available(time::Instant now) const noexcept
    -> time::Duration {
    return refills_.available(now);
}

auto SchedulingContext::eligible(time::Instant now) const noexcept -> bool {
    return !available(now).empty();
}

auto SchedulingContext::next_refill() const noexcept
    -> libk::optional<time::Instant> {
    return refills_.next();
}

auto SchedulingContext::bind(
    object::ObjectHold<Thread>&& target,
    CpuId home_cpu) noexcept -> Result {
    if (!target || domain_ == nullptr) {
        return libk::unexpected(Error::NotAdmitted);
    }
    if (binding_) {
        return libk::unexpected(Error::AlreadyBound);
    }
    if (home_cpu != home_cpu_) {
        return libk::unexpected(Error::WrongCpu);
    }
    Thread& thread = target.get();
    if (thread.idle()
        || thread.state_ != Thread::State::Prepared
        || thread.binding_ != nullptr) {
        return libk::unexpected(Error::AlreadyBound);
    }

    Binding& relation = binding_.emplace(
        *this, libk::move(target), home_cpu);
    thread.binding_ = &relation;
    return libk::expected();
}

auto SchedulingContext::unbind() noexcept -> Result {
    if (!binding_) {
        return libk::unexpected(Error::NotBound);
    }
    if (active_cpu_ || binding_->queued()) {
        return libk::unexpected(Error::Active);
    }
    Thread& thread = binding_->thread();
    if (thread.state_ == Thread::State::Running
        || thread.state_ == Thread::State::Ready
        || thread.state_ == Thread::State::Throttled) {
        return libk::unexpected(Error::Active);
    }
    KASSERT(thread.binding_ == &*binding_);
    thread.binding_ = nullptr;
    binding_.reset();
    return libk::expected();
}

auto SchedulingContext::activate(CpuId cpu) noexcept -> bool {
    if (active_cpu_ || cpu != home_cpu_ || domain_ == nullptr) {
        return false;
    }
    active_cpu_ = cpu;
    ++activation_count_;
    KASSERT(activation_count_ != 0);
    return true;
}

void SchedulingContext::deactivate(CpuId cpu) noexcept {
    KASSERT(active_cpu_ && *active_cpu_ == cpu);
    active_cpu_.reset();
}

void SchedulingContext::charge(
    time::Instant now,
    time::Duration elapsed) noexcept {
    const time::Duration overrun = refills_.charge(now, elapsed);
    if (!overrun.empty()) {
        const auto total = libk::checked_add(
            overrun_.ticks(), overrun.ticks());
        KASSERT(total);
        overrun_ = time::Duration::from_ticks(*total);
    }
}

} // namespace kernel::sched
