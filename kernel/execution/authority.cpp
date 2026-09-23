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

template<usize Index>
auto Authority::ops() noexcept -> const cap::GrantAttachmentOps& {
    static constexpr cap::GrantAttachmentOps operations{
        .invalidate = [](void* context, cap::GrantWork&& work, cap::GrantInvalidation reason) noexcept {
            KASSERT(reason == cap::GrantInvalidation::Revoke);
            static_cast<Authority*>(context)->invalidate(Index, libk::move(work));
        },
        .released = [](void* context) noexcept { static_cast<Authority*>(context)->released(Index); },
    };
    return operations;
}

Authority::Authority(Thread& thread) noexcept
    : target_(libk::in_place_type<Thread*>, &thread),
      links_{{*this, ops<VSpace>()}, {*this, ops<CSpace>()}, {*this, ops<Control>()},
             {*this, ops<Events>()}, {*this, ops<Code>()}, {*this, ops<Stack>()}},
      stop_(Stop::Notifier::bind<&Authority::stopped>(*this)) {}

Authority::Authority(Vproc& vproc) noexcept
    : target_(libk::in_place_type<Vproc*>, &vproc),
      links_{{*this, ops<VSpace>()}, {*this, ops<CSpace>()}, {*this, ops<Control>()},
             {*this, ops<Events>()}, {*this, ops<Code>()}, {*this, ops<Stack>()}},
      stop_(Stop::Notifier::bind<&Authority::stopped>(*this)) {}

Authority::~Authority() noexcept {
    KASSERT(!cleanup_ && pending_ == 0 && !attaching_ && !start_armed_);
    KASSERT(!stop_.started() || stop_.complete());
}

template<usize First, class A, class B>
auto Authority::attach_pair(const cap::Resolved<A>& first, const cap::Resolved<B>& second) noexcept
    -> libk::Expected<void, cap::GrantError> {
    constexpr u8 pair = 3U << First;
    constexpr u8 required = (1U << First) - 1;
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (attaching_ || ended_ || start_armed_ || stop_.started()
            || (pending_ & pair) != 0 || (pending_ & required) != required)
            return libk::unexpected(cap::GrantError::InvalidState);
        attaching_ = true;
        pending_ |= pair;
        detached_ &= ~pair;
    }
    auto attached = first.attach(links_[First].attachment);
    const bool first_attached = static_cast<bool>(attached);
    if (attached) attached = second.attach(links_[First + 1].attachment);
    bool accepted{}, ended{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (!first_attached) pending_ &= ~(1U << First);
        if (!attached) pending_ &= ~(1U << (First + 1));
        attaching_ = false;
        ended = ended_;
        accepted = attached && !ended && !start_armed_ && !stop_.started();
    }
    if (!accepted) {
        detach(First);
        detach(First + 1);
    }
    if (ended) target_stopped();
    if (!attached) return attached;
    return accepted ? libk::Expected<void, cap::GrantError>{libk::expected()}
                    : libk::unexpected(cap::GrantError::InvalidState);
}

auto Authority::attach(const cap::Resolved<mm::VSpace>& vspace,
    const cap::Resolved<cap::CSpace>& cspace) noexcept -> libk::Expected<void, cap::GrantError> {
    return attach_pair<VSpace>(vspace, cspace);
}
auto Authority::attach_runtime(const cap::Resolved<mm::MemoryObject>& control,
    const cap::Resolved<mm::MemoryObject>& events) noexcept -> libk::Expected<void, cap::GrantError> {
    return attach_pair<Control>(control, events);
}
auto Authority::attach_arm(const cap::Resolved<mm::MemoryObject>& code,
    const cap::Resolved<mm::MemoryObject>& stack) noexcept -> libk::Expected<void, cap::GrantError> {
    return attach_pair<Code>(code, stack);
}

void Authority::detach(usize index) noexcept {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        const u8 bit = 1U << index;
        if ((pending_ & bit) == 0 || (detached_ & bit) != 0) return;
        detached_ |= bit;
    }
    if (links_[index].attachment.detach()) released(index);
}

void Authority::detach_arm() noexcept {
    detach(Code);
    detach(Stack);
    for (usize index = Code; index <= Stack; ++index) {
        auto& attachment = links_[index].attachment;
        if (!attachment.attached() && !attachment.busy()) attachment.reset();
    }
}

auto Authority::active() const noexcept -> bool {
    kernel::sync::IrqLockGuard guard{lock_};
    return pending_ != 0 || attaching_ || start_armed_;
}

void Authority::released(usize index) noexcept {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT((pending_ & (1U << index)) != 0);
        pending_ &= ~(1U << index);
    }
    finish_retire();
}

void Authority::invalidate(usize index, cap::GrantWork&& work) noexcept {
    bool start{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        auto& link = links_[index];
        KASSERT(!link.work);
        link.work = libk::move(work);
        start = !ended_ && !stop_.started() && !start_armed_;
        if (start) start_armed_ = true;
    }
    if (start) {
        start_stop();
        bool ended{};
        {
            kernel::sync::IrqLockGuard guard{lock_};
            ended = ended_;
        }
        if (ended) drain(links_[index]);
        {
            kernel::sync::IrqLockGuard guard{lock_};
            start_armed_ = false;
        }
        finish_retire();
        return;
    }
    bool ended{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        ended = ended_;
    }
    // The last release can finish object cleanup; do not touch this afterward.
    if (ended) drain(links_[index]);
}

void Authority::start_stop() noexcept {
    libk::visit([this](auto* target) noexcept { stop_.start(*target); }, target_);
}
void Authority::stopped() noexcept { target_stopped(); }

void Authority::target_stopped() noexcept {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        ended_ = true;
        if (attaching_) return;
    }
    for (usize index = 0; index < Count; ++index) detach(index);
    for (auto& link : links_) drain(link);
    finish_retire();
}

void Authority::drain(Link& link) noexcept {
    cap::GrantWork work{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        work = libk::move(link.work);
    }
    work.reset();
}

void Authority::retire(object::ObjectCleanup&& cleanup) noexcept {
    target_stopped();
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(!cleanup_);
        cleanup_ = libk::move(cleanup);
    }
    finish_retire();
}

void Authority::finish_retire() noexcept {
    object::ObjectCleanup done;
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (!cleanup_ || pending_ != 0 || attaching_ || start_armed_
            || (stop_.started() && !stop_.complete())) return;
        done = libk::move(cleanup_);
    }
    done.complete();
}

} // namespace kernel::execution
