#include <execution/authority.hpp>

#include <core/debug.hpp>
#include <execution/vproc.hpp>
#include <libk/utility.hpp>
#include <object/cspace_pool.hpp>
#include <object/memory_pool.hpp>
#include <object/vspace_pool.hpp>
#include <sync/irq_lock_guard.hpp>
#include <thread/thread.hpp>

namespace kernel::execution {

const cap::GrantAttachmentOps Authority::ops_{
    .invalidate = &Authority::invalidate,
    .released = &Authority::released,
};

Authority::Authority(Thread& thread) noexcept
    : target_(libk::in_place_type<Thread*>, &thread),
      vspace_(*this, ops_),
      cspace_(*this, ops_),
      control_(*this, ops_),
      events_(*this, ops_),
      stop_(Stop::Notifier::bind<&Authority::stopped>(*this)) {}

Authority::Authority(Vproc& vproc) noexcept
    : target_(libk::in_place_type<Vproc*>, &vproc),
      vspace_(*this, ops_),
      cspace_(*this, ops_),
      control_(*this, ops_),
      events_(*this, ops_),
      stop_(Stop::Notifier::bind<&Authority::stopped>(*this)) {}

Authority::~Authority() noexcept {
    reset();
}

auto Authority::attach(
    const cap::Resolved<kernel::mm::VSpace>& vspace,
    const cap::Resolved<cap::CSpace>& cspace) noexcept
    -> libk::Expected<void, cap::GrantError> {
    if (active() || stop_.started()) {
        return libk::unexpected(cap::GrantError::InvalidState);
    }
    auto attached = vspace.attach(vspace_.attachment);
    if (!attached) {
        return attached;
    }
    attached = cspace.attach(cspace_.attachment);
    if (!attached) {
        KASSERT(vspace_.attachment.detach());
        return attached;
    }
    return libk::expected();
}

auto Authority::attach_runtime(
    const cap::Resolved<kernel::mm::MemoryObject>& control,
    const cap::Resolved<kernel::mm::MemoryObject>& events) noexcept
    -> libk::Expected<void, cap::GrantError> {
    if (!vspace_.attachment.attached() || !cspace_.attachment.attached()
        || control_.attachment.attached() || events_.attachment.attached()
        || stop_.started()) {
        return libk::unexpected(cap::GrantError::InvalidState);
    }
    auto attached = control.attach(control_.attachment);
    if (!attached) {
        return attached;
    }
    attached = events.attach(events_.attachment);
    if (!attached) {
        KASSERT(control_.attachment.detach());
        return attached;
    }
    return libk::expected();
}

auto Authority::active() const noexcept -> bool {
    const auto live = [](const Link& link) noexcept {
        return link.attachment.attached() || link.attachment.busy();
    };
    return live(vspace_) || live(cspace_) || live(control_) || live(events_);
}

void Authority::invalidate(
    void* context,
    cap::GrantWork&& work,
    cap::GrantInvalidation reason) noexcept {
    KASSERT(context != nullptr && reason == cap::GrantInvalidation::Revoke);
    auto& link = *static_cast<Link*>(context);
    KASSERT(link.owner != nullptr);
    link.owner->invalidate(link, libk::move(work));
}

void Authority::released(void* context) noexcept {
    KASSERT(context != nullptr);
    auto& link = *static_cast<Link*>(context);
    KASSERT(link.owner != nullptr);
}

void Authority::invalidate(Link& link, cap::GrantWork&& work) noexcept {
    bool start{};
    bool already_stopped{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(!link.work);
        link.work = libk::move(work);
        already_stopped = stop_.complete();
        start = !stop_.started() && !start_armed_;
        if (start) {
            start_armed_ = true;
        }
    }
    if (start) {
        start_stop();
        kernel::sync::IrqLockGuard guard{lock_};
        start_armed_ = false;
    }
    if (already_stopped || stop_.complete()) {
        drain(link);
    }
}

void Authority::start_stop() noexcept {
    libk::visit([this](auto* target) noexcept {
        KASSERT(target != nullptr);
        stop_.start(*target);
    }, target_);
}

void Authority::stopped() noexcept {
    target_stopped();
}

void Authority::target_stopped() noexcept {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (ended_) {
            return;
        }
        ended_ = true;
    }
    Link* const links[] = {&vspace_, &cspace_, &control_, &events_};
    for (Link* const link : links) {
        if (link->attachment.attached()) {
            static_cast<void>(link->attachment.detach());
        }
    }
    for (Link* const link : links) {
        drain(*link);
    }
}

void Authority::drain(Link& link) noexcept {
    cap::GrantWork work{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        work = libk::move(link.work);
    }
    work.reset();
}

void Authority::reset() noexcept {
    target_stopped();
    KASSERT(!stop_.started() || stop_.complete());
    KASSERT(!start_armed_);
}

} // namespace kernel::execution
