#include <sched/authority.hpp>

#include <core/debug.hpp>
#include <libk/utility.hpp>
#include <object/sched_pool.hpp>
#include <object/thread_pool.hpp>
#include <sync/irq_lock_guard.hpp>
#include <thread/thread.hpp>
#include <execution/vproc.hpp>

namespace kernel::sched {

const cap::GrantAttachmentOps BindingAuthority::ops_{
    .invalidate = &BindingAuthority::invalidate,
    .released = &BindingAuthority::released,
};

BindingAuthority::BindingAuthority(Thread& thread) noexcept
    : target_(execution::Target{thread}),
      context_(*this, ops_),
      target_cap_(*this, ops_),
      stop_(execution::Stop::Notifier::bind<
          &BindingAuthority::stopped>(*this)) {}

BindingAuthority::BindingAuthority(Vproc& vproc) noexcept
    : target_(execution::Target{vproc}),
      context_(*this, ops_),
      target_cap_(*this, ops_),
      stop_(execution::Stop::Notifier::bind<
          &BindingAuthority::stopped>(*this)) {}

BindingAuthority::~BindingAuthority() noexcept {
    release();
    KASSERT(!stop_.started() || stop_.complete());
    KASSERT(!start_armed_);
}

auto BindingAuthority::attach(
    const cap::Resolved<SchedulingContext>& context,
    const cap::Resolved<Thread>& thread) noexcept
    -> libk::Expected<void, cap::GrantError> {
    if (ended_ || context_.attachment.attached()
        || target_cap_.attachment.attached()) {
        return libk::unexpected(cap::GrantError::InvalidState);
    }
    auto attached = context.attach(context_.attachment);
    if (!attached) {
        return attached;
    }
    attached = thread.attach(target_cap_.attachment);
    if (!attached) {
        KASSERT(context_.attachment.detach());
        return attached;
    }
    return libk::expected();
}

auto BindingAuthority::attach(
    const cap::Resolved<SchedulingContext>& context,
    const cap::Resolved<Vproc>& vproc) noexcept
    -> libk::Expected<void, cap::GrantError> {
    if (ended_ || context_.attachment.attached()
        || target_cap_.attachment.attached()) {
        return libk::unexpected(cap::GrantError::InvalidState);
    }
    auto attached = context.attach(context_.attachment);
    if (!attached) {
        return attached;
    }
    attached = vproc.attach(target_cap_.attachment);
    if (!attached) {
        KASSERT(context_.attachment.detach());
        return attached;
    }
    return libk::expected();
}

auto BindingAuthority::reusable() const noexcept -> bool {
    kernel::sync::IrqLockGuard guard{lock_};
    return ended_ && (!stop_.started() || stop_.complete())
        && !start_armed_ && !context_.attachment.busy()
        && !target_cap_.attachment.busy();
}

void BindingAuthority::invalidate(
    void* context,
    cap::GrantWork&& work,
    cap::GrantInvalidation reason) noexcept {
    KASSERT(context != nullptr && reason == cap::GrantInvalidation::Revoke);
    auto& link = *static_cast<Link*>(context);
    KASSERT(link.owner != nullptr);
    link.owner->invalidate(link, libk::move(work));
}

void BindingAuthority::released(void* context) noexcept {
    KASSERT(context != nullptr);
}

void BindingAuthority::invalidate(
    Link& link,
    cap::GrantWork&& work) noexcept {
    bool start{};
    bool ended{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(!link.work);
        link.work = libk::move(work);
        ended = ended_;
        start = !ended && !stop_.started() && !start_armed_;
        if (start) {
            start_armed_ = true;
        }
    }
    if (start) {
        if (Thread* const thread = target_.thread()) {
            stop_.start(*thread);
        } else {
            stop_.start(*target_.vproc());
        }
        kernel::sync::IrqLockGuard guard{lock_};
        start_armed_ = false;
    }
    if (ended || stop_.complete()) {
        drain(link);
    }
}

void BindingAuthority::stopped() noexcept {
    release();
}

void BindingAuthority::release() noexcept {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (ended_) {
            return;
        }
        ended_ = true;
    }
    if (context_.attachment.attached()) {
        static_cast<void>(context_.attachment.detach());
    }
    if (target_cap_.attachment.attached()) {
        static_cast<void>(target_cap_.attachment.detach());
    }
    drain(context_);
    drain(target_cap_);
}

void BindingAuthority::drain(Link& link) noexcept {
    cap::GrantWork work{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        work = libk::move(link.work);
    }
    work.reset();
}

} // namespace kernel::sched
