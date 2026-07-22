#include <sched/context.hpp>

#include <core/debug.hpp>
#include <libk/checked_arithmetic.hpp>
#include <libk/utility.hpp>
#include <object/sched_pool.hpp>
#include <object/thread_pool.hpp>
#include <object/vproc_pool.hpp>
#include <sched/domain.hpp>
#include <sync/irq_lock_guard.hpp>
#include <thread/thread.hpp>

namespace kernel::sched {

const cap::GrantAttachmentOps SchedulingContext::domain_ops_{
    .invalidate = &SchedulingContext::invalidate_domain,
    .released = &SchedulingContext::released_domain,
};

SchedulingContext::SchedulingContext(Config config, time::Instant now) noexcept
    : config_(config),
      refills_(config.budget, config.period, config.refill_capacity, now) {
    KASSERT(valid_config(config_));
}

SchedulingContext::~SchedulingContext() noexcept {
    KASSERT(!active_cpu_);
    KASSERT(!binding_);
    KASSERT(!binding_authority_);
    KASSERT(domain_ == nullptr);
    KASSERT(!domain_authority_.attached() && !domain_authority_.busy());
    KASSERT(!domain_work_);
    KASSERT(!domain_stop_.started() || domain_stop_.complete());
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

auto SchedulingContext::admit(
    cap::Resolved<SchedulingDomain>& authority,
    CpuId home_cpu) noexcept -> Result {
    if (domain_ != nullptr || domain_authority_.attached() || withdrawing_) {
        return libk::unexpected(Error::AlreadyAdmitted);
    }
    auto attached = authority.attach(domain_authority_);
    if (!attached) {
        return libk::unexpected(Error::InvalidConfig);
    }
    auto admitted = authority.object().admit(*this, home_cpu);
    if (!admitted) {
        KASSERT(domain_authority_.detach());
        switch (admitted.error()) {
        case SchedulingDomain::Error::InvalidCpu:
            return libk::unexpected(Error::WrongCpu);
        case SchedulingDomain::Error::Busy:
            return libk::unexpected(Error::AlreadyAdmitted);
        default:
            return libk::unexpected(Error::InvalidConfig);
        }
    }
    return libk::expected();
}

auto SchedulingContext::bind(
    object::ObjectHold<Thread>&& target) noexcept -> Result {
    return bind_target(execution::TargetHold{libk::move(target)});
}

auto SchedulingContext::bind(
    object::ObjectHold<Vproc>&& target) noexcept -> Result {
    return bind_target(execution::TargetHold{libk::move(target)});
}

auto SchedulingContext::bind_target(
    execution::TargetHold&& target) noexcept -> Result {
    if (!target || domain_ == nullptr) {
        return libk::unexpected(Error::NotAdmitted);
    }
    kernel::sync::IrqLockGuard authority_guard{authority_lock_};
    if (withdrawing_) {
        return libk::unexpected(Error::Active);
    }
    if (binding_) {
        return libk::unexpected(Error::AlreadyBound);
    }
    Binding& relation = binding_.emplace(
        *this, libk::move(target), home_cpu_);
    if (!relation.target().try_bind(relation)) {
        binding_.reset();
        return libk::unexpected(Error::AlreadyBound);
    }
    return libk::expected();
}

auto SchedulingContext::bind_authorized(
    object::ObjectHold<Thread>&& target,
    const cap::Resolved<SchedulingContext>& context,
    const cap::Resolved<Thread>& thread) noexcept -> Result {
    if (!target || &context.object() != this
        || &thread.object() != &target.get()) {
        return libk::unexpected(Error::InvalidConfig);
    }
    if (binding_authority_) {
        if (!binding_authority_->reusable()) {
            return libk::unexpected(Error::Active);
        }
        binding_authority_.reset();
    }
    auto& authority = binding_authority_.emplace(target.get());
    if (!authority.attach(context, thread)) {
        binding_authority_.reset();
        return libk::unexpected(Error::Active);
    }
    auto bound = bind(libk::move(target));
    if (!bound) {
        authority.release();
        binding_authority_.reset();
    }
    return bound;
}

auto SchedulingContext::bind_authorized(
    object::ObjectHold<Vproc>&& target,
    const cap::Resolved<SchedulingContext>& context,
    const cap::Resolved<Vproc>& vproc) noexcept -> Result {
    if (!target || &context.object() != this
        || &vproc.object() != &target.get()) {
        return libk::unexpected(Error::InvalidConfig);
    }
    if (binding_authority_) {
        if (!binding_authority_->reusable()) {
            return libk::unexpected(Error::Active);
        }
        binding_authority_.reset();
    }
    auto& authority = binding_authority_.emplace(target.get());
    if (!authority.attach(context, vproc)) {
        binding_authority_.reset();
        return libk::unexpected(Error::Active);
    }
    auto bound = bind(libk::move(target));
    if (!bound) {
        authority.release();
        binding_authority_.reset();
    }
    return bound;
}

auto SchedulingContext::unbind() noexcept -> Result {
    return unbind(nullptr);
}

auto SchedulingContext::unbind(CpuDispatcher* owner) noexcept -> Result {
    kernel::sync::IrqLockGuard authority_guard{authority_lock_};
    if (!binding_) {
        return libk::unexpected(Error::NotBound);
    }
    if (active_cpu_ || binding_->queued()) {
        return libk::unexpected(Error::Active);
    }
    execution::Target target = binding_->target();
    if (!target.release_binding(*binding_, owner)) {
        return libk::unexpected(Error::Active);
    }
    binding_.reset();
    if (binding_authority_) {
        binding_authority_->release();
    }
    return libk::expected();
}

auto SchedulingContext::prepare_retire() noexcept -> bool {
    {
        kernel::sync::IrqLockGuard guard{authority_lock_};
        if (active_cpu_ || binding_) {
            return false;
        }
        withdrawing_ = true;
    }
    if (domain_ != nullptr) {
        SchedulingDomain* const domain = domain_;
        if (!domain->unadmit(*this)) {
            return false;
        }
    }
    if (domain_authority_.attached()) {
        static_cast<void>(domain_authority_.detach());
    }
    domain_work_.reset();
    if (binding_authority_) {
        binding_authority_->release();
        if (!binding_authority_->reusable()) {
            return false;
        }
        binding_authority_.reset();
    }
    return !domain_authority_.busy();
}

auto SchedulingContext::startable() const noexcept -> bool {
    kernel::sync::IrqLockGuard guard{authority_lock_};
    return domain_ != nullptr && binding_ && !withdrawing_;
}

void SchedulingContext::invalidate_domain(
    void* context,
    cap::GrantWork&& work,
    cap::GrantInvalidation reason) noexcept {
    KASSERT(context != nullptr && reason == cap::GrantInvalidation::Revoke);
    static_cast<SchedulingContext*>(context)->invalidate_domain(
        libk::move(work));
}

void SchedulingContext::released_domain(void* context) noexcept {
    KASSERT(context != nullptr);
}

void SchedulingContext::invalidate_domain(cap::GrantWork&& work) noexcept {
    execution::Target target{};
    {
        kernel::sync::IrqLockGuard guard{authority_lock_};
        KASSERT(!domain_work_);
        domain_work_ = libk::move(work);
        withdrawing_ = true;
        if (binding_) {
            target = binding_->target();
        }
    }
    if (target) {
        if (!domain_stop_.started()) {
            if (Thread* const thread = target.thread()) {
                domain_stop_.start(*thread);
            } else {
                domain_stop_.start(*target.vproc());
            }
        }
        return;
    }
    finish_domain();
}

void SchedulingContext::finish_domain() noexcept {
    SchedulingDomain* domain{};
    {
        kernel::sync::IrqLockGuard guard{authority_lock_};
        KASSERT(withdrawing_ && !binding_ && !active_cpu_);
        domain = domain_;
    }
    if (domain != nullptr) {
        KASSERT(domain->unadmit(*this));
    }
    if (domain_authority_.attached()) {
        static_cast<void>(domain_authority_.detach());
    }
    domain_work_.reset();
}

auto SchedulingContext::activate(CpuId cpu) noexcept -> bool {
    kernel::sync::IrqLockGuard guard{authority_lock_};
    if (active_cpu_ || cpu != home_cpu_ || domain_ == nullptr
        || withdrawing_) {
        return false;
    }
    active_cpu_ = cpu;
    ++activation_count_;
    KASSERT(activation_count_ != 0);
    return true;
}

void SchedulingContext::deactivate(CpuId cpu) noexcept {
    kernel::sync::IrqLockGuard guard{authority_lock_};
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
