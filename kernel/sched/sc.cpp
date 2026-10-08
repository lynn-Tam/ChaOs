#include <expected>
#include <algorithm>
#include <optional>
#include <sched/sc.hpp>

#include <libk/assert.hpp>
#include <base/types.hpp>
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
    .released = [](void*) noexcept {},
};

Sc::auth::auth(Thread& target) noexcept
    : target_(&target), context_(*this, ops_), target_cap_(*this, ops_),
      stop_(Stop::Notifier::bind<&auth::release>(*this)) {}

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
        && (!revoking_ || stop_.complete())
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
        if (start) revoking_ = true;
    }
    if (start) stop_.start(*target_);
    if (ended || stop_.complete()) drain(cell);
}

void Sc::auth::release() noexcept {
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

Sc::Sc(Config config, time::Instant now) noexcept
    : config_(config) {
    libk_assert(valid_config(config_));
    libk_assert(refills_.try_emplace_back(Refill{now, config_.budget}));
}

Sc::~Sc() noexcept {
    libk_assert(!mailed_.load<libk::MemoryOrder::Acquire>() && !mail_hook_.is_linked());
    libk_assert(!active());
    libk_assert(!owner_);
    libk_assert(!auth_);
    libk_assert(domain_ == nullptr);
    libk_assert(!domain_authority_.attached() && !domain_authority_.busy());
    libk_assert(!domain_work_);
    libk_assert(!domain_stop_.started() || domain_stop_.complete());
}

auto Sc::available(time::Instant now) const noexcept
    -> time::Duration {
    u64 total{};
    for (const auto& r : refills_) {
        if (r.ready_at > now) break;
        total += r.amount.ticks();
    }
    return time::Duration::from_ticks(total);
}

auto Sc::eligible(time::Instant now) const noexcept -> bool {
    return !available(now).empty();
}

auto Sc::admit(
    cap::Resolved<Domain>& authority,
    CpuId home_cpu) noexcept -> Result {
    if (domain_ != nullptr || domain_authority_.attached() || withdrawing_) {
        return std::unexpected(Error::AlreadyAdmitted);
    }
    auto attached = authority.attach(domain_authority_);
    if (!attached) {
        return std::unexpected(Error::InvalidConfig);
    }
    auto admitted = authority.object().admit(*this, home_cpu);
    if (!admitted) {
        libk_assert(domain_authority_.detach());
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

auto Sc::bind(
    object::ref<Thread>&& owner) noexcept -> Result {
    if (!owner || domain_ == nullptr) {
        return std::unexpected(Error::NotAdmitted);
    }
    sync::Lock authority_guard{authority_lock_};
    if (withdrawing_) {
        return std::unexpected(Error::Active);
    }
    if (owner_) {
        return std::unexpected(Error::AlreadyBound);
    }
    if (!owner->try_bind(*this)) {
        return std::unexpected(Error::AlreadyBound);
    }
    owner_ = std::move(owner);
    return {};
}

auto Sc::bind_authorized(
    object::ref<Thread>&& target,
    const cap::Resolved<Sc>& context,
    const cap::Resolved<Thread>& thread) noexcept -> Result {
    if (!target || &context.object() != this
        || &thread.object() != &target.get()) {
        return std::unexpected(Error::InvalidConfig);
    }
    if (auth_) {
        if (!auth_->reusable()) {
            return std::unexpected(Error::Active);
        }
        auth_.reset();
    }
    auto& authority = auth_.emplace(target.get());
    if (!authority.attach(context, thread)) {
        if (auth_->reusable()) auth_.reset();
        return std::unexpected(Error::Active);
    }
    auto bound = bind(std::move(target));
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
    sync::Lock authority_guard{authority_lock_};
    if (!owner_) {
        return std::unexpected(Error::NotBound);
    }
    if (active() || queued()) {
        return std::unexpected(Error::Active);
    }
    if (owner == nullptr && mailed_.load<libk::MemoryOrder::Acquire>())
        return std::unexpected(Error::Active);
    Thread& target = thread();
    if (!target.release_sc(*this, owner)) {
        return std::unexpected(Error::Active);
    }
    // release_sc closes publication under the execution lock. Drain requests
    // only after that cut, while owner_ still protects the execution lifetime.
    if (owner) owner->cancel(*this);
    libk_assert(!timer_queued());
    libk_assert(!mailed_.load<libk::MemoryOrder::Acquire>());
    object::ref<> lifetime{std::move(owner_)};
    wake_credit_ = false;
    if (auth_) {
        auth_->release();
    }
    return lifetime;
}

auto Sc::prepare_retire() noexcept -> bool {
    {
        sync::Lock guard{authority_lock_};
        if (active() || owner_) {
            return false;
        }
        withdrawing_ = true;
    }
    if (domain_ != nullptr) {
        Domain* const domain = domain_;
        if (!domain->unadmit(*this)) {
            return false;
        }
    }
    if (domain_authority_.attached()) {
        static_cast<void>(domain_authority_.detach());
    }
    domain_work_.reset();
    if (auth_) {
        auth_->release();
        if (!auth_->reusable()) {
            return false;
        }
        auth_.reset();
    }
    return !domain_authority_.busy();
}

auto Sc::startable() const noexcept -> bool {
    sync::Lock guard{authority_lock_};
    return domain_ != nullptr && owner_ && !withdrawing_;
}

void Sc::invalidate_domain(
    void* context,
    cap::GrantWork&& work) noexcept {
    libk_assert(context != nullptr);
    static_cast<Sc*>(context)->invalidate_domain(
        std::move(work));
}

void Sc::released_domain(void* context) noexcept {
    libk_assert(context != nullptr);
}

void Sc::invalidate_domain(cap::GrantWork&& work) noexcept {
    Thread* target{};
    {
        sync::Lock guard{authority_lock_};
        libk_assert(!domain_work_);
        domain_work_ = std::move(work);
        withdrawing_ = true;
        if (owner_) {
            target = &thread();
        }
    }
    if (target) {
        if (!domain_stop_.started()) {
            domain_stop_.start(*target);
        }
        return;
    }
    finish_domain();
}

void Sc::finish_domain() noexcept {
    Domain* domain{};
    {
        sync::Lock guard{authority_lock_};
        libk_assert(withdrawing_ && !owner_ && !active());
        domain = domain_;
    }
    if (domain != nullptr) {
        libk_assert(domain->unadmit(*this));
    }
    if (domain_authority_.attached()) {
        static_cast<void>(domain_authority_.detach());
    }
    domain_work_.reset();
}

auto Sc::activate(CpuId cpu) noexcept -> bool {
    sync::Lock guard{authority_lock_};
    if (active() || cpu != home_cpu_ || domain_ == nullptr
        || withdrawing_) {
        return false;
    }
    active_cpu_.store<libk::MemoryOrder::Release>(cpu.raw);
    return true;
}

void Sc::deactivate(CpuId cpu) noexcept {
    usize expected = cpu.raw;
    const bool deactivated = active_cpu_.compare_exchange_strong<
        libk::MemoryOrder::AcqRel,
        libk::MemoryOrder::Acquire>(expected, MaxCpus);
    libk_assert(deactivated);
}

auto Sc::charge(time::Instant now, time::Duration elapsed) noexcept -> time::Duration {
    libk_assert(!timer_queued());
    u64 remaining = elapsed.ticks(), consumed{};
    while (remaining && !refills_.empty() && refills_.front().ready_at <= now) {
        auto& r = refills_.front();
        const u64 n = std::min(remaining, r.amount.ticks());
        remaining -= n;
        consumed += n;
        r.amount = time::Duration::from_ticks(r.amount.ticks() - n);
        if (r.amount.empty()) refills_.pop_front();
    }
    if (consumed) {
        const auto ready = now.checked_add(config_.period);
        libk_assert(ready);
        // Total budget is conserved. Merging can delay a refill, never
        // advance it; sums are bounded by config_.budget, so cannot overflow.
        if (!refills_.empty() && (refills_.back().ready_at == *ready
            || refills_.size() == config_.refill_capacity)) {
            auto& last = refills_.back();
            libk_assert(last.ready_at <= *ready);
            last.ready_at = *ready;
            last.amount = time::Duration::from_ticks(last.amount.ticks() + consumed);
        } else libk_assert(refills_.try_emplace_back(Refill{*ready, time::Duration::from_ticks(consumed)}));
    }
    return time::Duration::from_ticks(remaining);
}

} // namespace sched
