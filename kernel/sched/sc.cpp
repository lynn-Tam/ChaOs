#include <expected>
#include <algorithm>
#include <optional>
#include <sched/sc.hpp>
#include <cpu/cpu.hpp>

#include <libk/assert.hpp>
#include <libk/checked_arithmetic.hpp>
#include <libk/types.hpp>
#include <utility>
#include <sched/domain.hpp>
#include <sched/dispatcher.hpp>
#include <object/ref.hpp>
#include <task/thread.hpp>
#include <sync.hpp>

namespace sched {

const cap::GrantAttachmentOps Sc::auth::ops_{
    .invalidate = [](void* context, cap::GrantWork&& work) noexcept {
        auto& cell = *static_cast<link*>(context);
        cell.owner->invalidate(cell, std::move(work));
    },
    .released = [](void* ctx) noexcept {
        static_cast<link*>(ctx)->owner->sc_->post_close();
    },
};

Sc::auth::auth(Sc& sc, Thread& target) noexcept
    : sc_(&sc), target_(&target), context_(*this, ops_), target_cap_(*this, ops_),
      stop_(Stop::Notifier::bind<&auth::stopped>(*this)) {}

Sc::auth::~auth() noexcept {
    release();
    libk_assert(reusable());
}

auto Sc::auth::attach(const cap::Resolved<Sc>& context,
                                    const cap::Resolved<Thread>& target) noexcept
    -> std::expected<void, cap::GrantError> {
    flight publishing{*this};
    {
        sync::Lock guard{lock_};
        if (ended_ || attaching_ || revoking_ || context_.attachment.attached()
            || target_cap_.attachment.attached())
            return std::unexpected(cap::GrantError::InvalidState);
        attaching_ = true;
    }
    auto result = context.attach(context_.attachment);
    if (result) result = target.attach(target_cap_.attachment);
    bool accepted{};
    {
        sync::Lock guard{lock_};
        attaching_ = false;
        accepted = result && !ended_ && !revoking_;
    }
    if (!accepted) release();
    if (!result) return result;
    return accepted ? std::expected<void, cap::GrantError>{}
                    : std::unexpected(cap::GrantError::InvalidState);
}

auto Sc::auth::reusable() const noexcept -> bool {
    sync::Lock guard{lock_};
    return ended_ && !attaching_ && publishers_ == 0
        && !stop_pending_
        && !context_.attachment.busy() && !target_cap_.attachment.busy();
}

void Sc::auth::invalidate(link& cell, cap::GrantWork&& work) noexcept {
    flight publishing{*this};
    bool start{}, ended{};
    {
        sync::Lock guard{lock_};
        libk_assert(!cell.work);
        cell.work = std::move(work);
        ended = ended_;
        start = !ended && !revoking_;
        if (start) revoking_ = stop_pending_ = true;
    }
    if (start) stop_.start(*target_);
    if (ended || stop_.complete()) drain(cell);
}

void Sc::auth::stopped() noexcept {
    flight publishing{*this};
    release();
    sync::Lock guard{lock_};
    libk_assert(stop_pending_);
    stop_pending_ = false;
}

void Sc::auth::release() noexcept {
    {
        sync::Lock guard{lock_};
        if (ended_ && context_.detaching && target_cap_.detaching) return;
    }
    flight publishing{*this};
    {
        sync::Lock guard{lock_};
        ended_ = true;
        if (attaching_) return;
    }
    for (auto* cell : {&context_, &target_cap_}) {
        bool detach{};
        {
            sync::Lock guard{lock_};
            detach = !cell->detaching;
            cell->detaching = true;
        }
        if (detach && cell->attachment.attached())
            static_cast<void>(cell->attachment.detach());
        drain(*cell);
    }
}

void Sc::auth::drain(link& cell) noexcept {
    cap::GrantWork work;
    {
        sync::Lock guard{lock_};
        work = std::move(cell.work);
    }
    work.reset();
}

const cap::GrantAttachmentOps Sc::domain_ops_{
    .invalidate = &Sc::invalidate_domain,
    .released = &Sc::released_domain,
};

Sc::Sc(Config config, u64 now) noexcept
    : config_(config) {
    libk_assert(valid_config(config_));
    libk_assert(refills_.try_emplace_back(Refill{now, config_.budget}));
}

Sc::~Sc() noexcept {
    libk_assert(!mailed_.load(std::memory_order_acquire) && !mail_hook_.is_linked());
    libk_assert(!active());
    libk_assert(!owner_);
    libk_assert(!auth_ && !cleanup_ && !stop_pending_);
    if (work_) work_->close(close_work_);
    libk_assert(domain_ == nullptr);
    libk_assert(!domain_cap_.attached() && !domain_cap_.busy());
    libk_assert(!domain_work_);
    libk_assert(!domain_stop_.started() || domain_stop_.complete());
}

void Sc::refill() noexcept {
    auto& d = current_cpu().dispatcher();
    libk_assert(home_cpu() == d.id() && thread().state() == Thread::State::Throttled);
    d.enqueue_or_throttle(*this, d.clock().rd_ticks());
}

auto Sc::available(u64 now) const noexcept
    -> u64 {
    u64 total{};
    for (const auto& r : refills_) {
        if (r.ready_at > now) break;
        total += r.amount;
    }
    return total;
}

auto Sc::eligible(u64 now) const noexcept -> bool {
    return available(now) != 0;
}

auto Sc::admit(
    cap::Resolved<Domain>& authority,
    CpuId home_cpu) noexcept -> Result {
    sync::Lock guard{lock_};
    if (domain_ != nullptr || domain_cap_.attached() || withdrawing_) {
        return std::unexpected(Error::AlreadyAdmitted);
    }
    auto attached = authority.attach(domain_cap_);
    if (!attached) {
        return std::unexpected(Error::InvalidConfig);
    }
    auto admitted = authority.object().admit(*this, home_cpu);
    if (!admitted) {
        libk_assert(domain_cap_.detach());
        switch (admitted.error()) {
        case Domain::Error::InvalidCpu:
            return std::unexpected(Error::WrongCpu);
        case Domain::Error::Busy:
            return std::unexpected(Error::AlreadyAdmitted);
        default:
            return std::unexpected(Error::InvalidConfig);
        }
    }
    return {};
}

auto Sc::bind(object::ref<Thread> owner) noexcept -> Result {
    sync::Lock guard{lock_};
    return bind_locked(std::move(owner));
}

auto Sc::bind_locked(object::ref<Thread> owner) noexcept -> Result {
    if (!owner || domain_ == nullptr) return std::unexpected(Error::NotAdmitted);
    if (withdrawing_) return std::unexpected(Error::Active);
    if (owner_ || !owner->try_bind(*this)) return std::unexpected(Error::AlreadyBound);
    owner_ = std::move(owner);
    return {};
}

auto Sc::bind_authorized(
    object::ref<Thread> target,
    const cap::Resolved<Sc>& context,
    const cap::Resolved<Thread>& thread) noexcept -> Result {
    if (!target || &context.object() != this
        || &thread.object() != &target.get()) {
        return std::unexpected(Error::InvalidConfig);
    }
    sync::Lock guard{lock_};
    if (withdrawing_) return std::unexpected(Error::Active);
    if (auth_) {
        if (!auth_->reusable()) {
            return std::unexpected(Error::Active);
        }
        auth_.reset();
    }
    auto& authority = auth_.emplace(*this, target.get());
    if (!authority.attach(context, thread)) {
        if (auth_->reusable()) auth_.reset();
        return std::unexpected(Error::Active);
    }
    auto bound = bind_locked(std::move(target));
    if (!bound) {
        authority.release();
        if (authority.reusable()) auth_.reset();
    }
    return bound;
}

auto Sc::unbind() noexcept
    -> std::expected<object::ref<>, Error> {
    return unbind(nullptr);
}

auto Sc::unbind(Dispatcher* owner) noexcept
    -> std::expected<object::ref<>, Error> {
    object::ref<> lifetime;
    auth* binding{};
    std::optional<auth::flight> publishing;
    {
        sync::Lock guard{lock_};
        if (!owner_) return std::unexpected(Error::NotBound);
        if (active() || queued()
            || (owner == nullptr && mailed_.load(std::memory_order_acquire)))
            return std::unexpected(Error::Active);
        if (!thread().release_sc(*this, owner)) return std::unexpected(Error::Active);
        // Cut publication before draining requests; owner_ protects execution
        // until the home consumer has finished its last queue access.
        if (owner) owner->cancel(*this);
        libk_assert(!refill_.armed() && !mailed_.load(std::memory_order_acquire));
        lifetime = object::ref<>{std::move(owner_)};
        wake_credit_ = false;
        if (auth_) {
            binding = &*auth_;
            publishing.emplace(*binding);
        }
    }
    // Releasing grant work may synchronously request this Sc's retirement.
    if (binding) binding->release();
    return lifetime;
}

void Sc::retire(object::cleanup&& done) noexcept {
    {
        sync::Lock guard{lock_};
        libk_assert(!cleanup_);
        cleanup_ = std::move(done);
        withdrawing_ = true;
    }
    post_close();
}

void Sc::stopped() noexcept {
    sync::Lock guard{lock_};
    libk_assert(stop_pending_);
    stop_pending_ = false;
    post_close();
}

void Sc::post_close() noexcept {
    libk_assert(work_);
    work_->post(close_work_);
}

auto Sc::startable() const noexcept -> bool {
    sync::Lock guard{lock_};
    return domain_ != nullptr && owner_ && !withdrawing_;
}

void Sc::invalidate_domain(
    void* context,
    cap::GrantWork&& work) noexcept {
    libk_assert(context != nullptr);
    static_cast<Sc*>(context)->invalidate_domain(
        std::move(work));
}

void Sc::released_domain(void* ctx) noexcept {
    static_cast<Sc*>(ctx)->post_close();
}

void Sc::invalidate_domain(cap::GrantWork&& work) noexcept {
    {
        sync::Lock guard{lock_};
        libk_assert(!domain_work_);
        domain_work_ = std::move(work);
        withdrawing_ = true;
    }
    post_close();
}

void Sc::finish_close() noexcept {
    object::ref<Thread> target;
    {
        sync::Lock guard{lock_};
        if (!withdrawing_) return;
        if (owner_) {
            if (domain_stop_.started()) return;
            target = owner_;
            stop_pending_ = true;
        } else {
            if (active() || stop_pending_) return;
            libk_assert(!queued() && !refill_.armed()
                && !mailed_.load(std::memory_order_acquire));
        }
    }
    if (target) {
        domain_stop_.start(target.get());
        return;
    }
    // Admission is closed and unbind has returned. Grant completion can enter
    // retire(), so drain relationships outside the Sc lock on this sole runner.
    if (domain_) libk_assert(domain_->unadmit(*this));
    if (domain_cap_.attached()) static_cast<void>(domain_cap_.detach());
    domain_work_.reset();
    if (auth_) {
        auth_->release();
        if (!auth_->reusable()) return;
        auth_.reset();
    }
    if (domain_cap_.busy()) return;
    object::cleanup done;
    {
        sync::Lock guard{lock_};
        done = std::move(cleanup_);
    }
    // Finalizers share this runner. Callback publishers have drained before
    // closing the Work; completing cleanup may enqueue storage reclamation.
    if (done) {
        work_->close(close_work_);
        done.complete();
    }
}

auto Sc::activate(CpuId cpu) noexcept -> bool {
    sync::Lock guard{lock_};
    if (active() || cpu != home_cpu_ || domain_ == nullptr
        || withdrawing_) {
        return false;
    }
    active_cpu_.store(cpu.raw, std::memory_order_release);
    return true;
}

void Sc::deactivate(CpuId cpu) noexcept {
    usize expected = cpu.raw;
    const bool deactivated = active_cpu_.compare_exchange_strong(expected, MaxCpus, std::memory_order_acq_rel, std::memory_order_acquire);
    libk_assert(deactivated);
}

auto Sc::charge(u64 now, u64 elapsed) noexcept -> u64 {
    libk_assert(!refill_.armed());
    u64 remaining = elapsed, consumed{};
    while (remaining && !refills_.empty() && refills_.front().ready_at <= now) {
        auto& r = refills_.front();
        const u64 n = std::min(remaining, r.amount);
        remaining -= n;
        consumed += n;
        r.amount -= n;
        if (r.amount == 0) refills_.pop_front();
    }
    if (consumed) {
        const auto ready = libk::checked_add(now, config_.period);
        libk_assert(ready);
        // Total budget is conserved. Merging can delay a refill, never
        // advance it; sums are bounded by config_.budget, so cannot overflow.
        if (!refills_.empty() && (refills_.back().ready_at == *ready
            || refills_.size() == config_.refill_capacity)) {
            auto& last = refills_.back();
            libk_assert(last.ready_at <= *ready);
            last.ready_at = *ready;
            last.amount += consumed;
        } else libk_assert(refills_.try_emplace_back(Refill{*ready, consumed}));
    }
    return remaining;
}

} // namespace sched
