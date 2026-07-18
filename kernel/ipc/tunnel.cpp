#include <ipc/tunnel.hpp>

#include <core/debug.hpp>
#include <cpu/cpu_registry.hpp>
#include <execution/vproc.hpp>
#include <libk/limits.hpp>
#include <libk/utility.hpp>
#include <object/vproc_pool.hpp>
#include <sched/dispatcher.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::ipc {

const cap::GrantAttachmentOps Tunnel::authority_ops_{
    .invalidate = &Tunnel::invalidate,
    .released = &Tunnel::released,
};

Tunnel::Tunnel(
    CpuRegistry& cpus,
    Vproc& source,
    Vproc& target,
    usize slot,
    usize tag) noexcept
    : cpus_(&cpus),
      source_(&source),
      target_(&target),
      slot_(slot),
      tag_(tag),
      source_authority_(*this, authority_ops_),
      target_authority_(*this, authority_ops_),
      source_link_(*this),
      target_link_(*this) {}

Tunnel::~Tunnel() noexcept {
    KASSERT(state_ == State::Constructing || state_ == State::Closed);
    KASSERT(!source_link_.hook.is_linked()
        && !target_link_.hook.is_linked());
    KASSERT(!source_authority_.attachment.attached()
        && !source_authority_.attachment.busy());
    KASSERT(!target_authority_.attachment.attached()
        && !target_authority_.attachment.busy());
    KASSERT(!source_hold_ && !target_hold_);
    KASSERT(relations_.load<libk::MemoryOrder::Acquire>() == 0);
    KASSERT(!cleanup_);
}

auto Tunnel::authorize(
    const cap::Resolved<Vproc>& source,
    const cap::Resolved<Vproc>& target) noexcept
    -> libk::Expected<void, TunnelError> {
    if (&source.object() != source_ || &target.object() != target_
        || source_ == target_ || slot_ >= MYOS_VPROC_MAX_INGRESS) {
        return libk::unexpected(TunnelError::InvalidSlot);
    }
    auto source_ref = source.reference();
    auto target_ref = target.reference();
    if (!source_ref || !target_ref) {
        return libk::unexpected(TunnelError::Busy);
    }
    auto source_hold =
        libk::move(source_ref).value().into_hold<Vproc>();
    auto target_hold =
        libk::move(target_ref).value().into_hold<Vproc>();
    if (!source_hold || !target_hold) {
        return libk::unexpected(TunnelError::Busy);
    }
    kernel::sync::IrqLockGuard guard{lock_};
    if (state_ != State::Constructing) {
        return libk::unexpected(TunnelError::Busy);
    }
    if (!source.attach(source_authority_.attachment)) {
        return libk::unexpected(TunnelError::Busy);
    }
    if (!target.attach(target_authority_.attachment)) {
        KASSERT(source_authority_.attachment.detach());
        return libk::unexpected(TunnelError::Busy);
    }
    if (!source_->attach_tunnel_source(source_link_)) {
        KASSERT(target_authority_.attachment.detach());
        KASSERT(source_authority_.attachment.detach());
        return libk::unexpected(TunnelError::Busy);
    }
    const auto generation = target_->attach_tunnel_target(
        target_link_, slot_, tag_);
    if (!generation) {
        source_->detach_tunnel_source(source_link_);
        KASSERT(target_authority_.attachment.detach());
        KASSERT(source_authority_.attachment.detach());
        return libk::unexpected(TunnelError::Busy);
    }
    binding_generation_ = *generation;
    source_hold_ = libk::move(source_hold).value();
    target_hold_ = libk::move(target_hold).value();
    state_ = State::Open;
    return libk::expected();
}

auto Tunnel::invoke(Vproc& caller) noexcept
    -> libk::Expected<u64, TunnelError> {
    object::ObjectPin<Vproc> target_pin{};
    u64 generation{};
    bool publish{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != State::Open) {
            return libk::unexpected(TunnelError::Closed);
        }
        if (&caller != source_) {
            return libk::unexpected(TunnelError::WrongSource);
        }
        if (!pending_) {
            auto target_ref = target_hold_.ref();
            if (!target_ref) {
                return libk::unexpected(TunnelError::Busy);
            }
            auto pinned = target_ref.value().pin<Vproc>();
            if (!pinned) {
                return libk::unexpected(TunnelError::Busy);
            }
            target_pin = libk::move(pinned).value();
            if (delivery_generation_ == libk::numeric_limits<u64>::max()) {
                return libk::unexpected(TunnelError::GenerationExhausted);
            }
            ++delivery_generation_;
            pending_ = true;
            publish = true;
        }
        generation = delivery_generation_;
    }
    if (publish) {
        target_pin->publish_tunnel(
            target_link_, slot_, binding_generation_, generation, tag_, *cpus_);
    }
    return libk::expected(generation);
}

auto Tunnel::take(Vproc& caller) noexcept
    -> libk::Expected<u64, TunnelError> {
    u64 generation{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != State::Open) {
            return libk::unexpected(TunnelError::Closed);
        }
        if (&caller != target_) {
            return libk::unexpected(TunnelError::WrongTarget);
        }
        if (!pending_) {
            return libk::unexpected(TunnelError::Empty);
        }
        pending_ = false;
        generation = delivery_generation_;
    }
    caller.clear_tunnel(
        target_link_, slot_, binding_generation_, generation);
    return libk::expected(generation);
}

void Tunnel::close() noexcept {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ == State::Closed || state_ == State::Closing) {
            return;
        }
        state_ = State::Closing;
        pending_ = false;
    }
    finish_close();
}

void Tunnel::finish_close() noexcept {
    source_->detach_tunnel_source(source_link_);
    target_->detach_tunnel_target(
        target_link_, slot_, binding_generation_);
    if (source_authority_.attachment.attached()) {
        static_cast<void>(source_authority_.attachment.detach());
    }
    if (target_authority_.attachment.attached()) {
        static_cast<void>(target_authority_.attachment.detach());
    }
    source_hold_.reset();
    target_hold_.reset();
    cap::GrantWork source_work{};
    cap::GrantWork target_work{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        source_work = libk::move(source_authority_.work);
        target_work = libk::move(target_authority_.work);
        state_ = State::Closed;
    }
    source_work.reset();
    target_work.reset();
    try_finish_retire();
}

void Tunnel::retire(object::ObjectCleanup&& cleanup) noexcept {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(!cleanup_);
        cleanup_ = libk::move(cleanup);
    }
    close();
    try_finish_retire();
}

void Tunnel::retain_relation() noexcept {
    const usize previous = relations_.fetch_add<libk::MemoryOrder::AcqRel>(1);
    KASSERT(previous != libk::numeric_limits<usize>::max());
}

void Tunnel::release_relation() noexcept {
    const usize previous = relations_.fetch_sub<libk::MemoryOrder::AcqRel>(1);
    KASSERT(previous != 0);
    if (previous == 1) {
        try_finish_retire();
    }
}

void Tunnel::peer_stopped(Vproc& peer) noexcept {
    KASSERT(&peer == source_ || &peer == target_);
    close();
}

void Tunnel::invalidate(
    void* context,
    cap::GrantWork&& work,
    cap::GrantInvalidation reason) noexcept {
    KASSERT(context != nullptr && reason == cap::GrantInvalidation::Revoke);
    auto& link = *static_cast<AuthorityLink*>(context);
    link.owner->invalidated(link, libk::move(work));
}

void Tunnel::released(void* context) noexcept {
    KASSERT(context != nullptr);
}

void Tunnel::invalidated(
    AuthorityLink& link,
    cap::GrantWork&& work) noexcept {
    bool closed{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(!link.work);
        link.work = libk::move(work);
        closed = state_ == State::Closed;
    }
    if (closed) {
        cap::GrantWork completed{};
        {
            kernel::sync::IrqLockGuard guard{lock_};
            completed = libk::move(link.work);
        }
        completed.reset();
        try_finish_retire();
        return;
    }
    close();
}

void Tunnel::try_finish_retire() noexcept {
    object::ObjectCleanup cleanup{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != State::Closed || !cleanup_
            || relations_.load<libk::MemoryOrder::Acquire>() != 0
            || source_authority_.work || target_authority_.work) {
            return;
        }
        cleanup = libk::move(cleanup_);
    }
    cleanup.complete();
}

} // namespace kernel::ipc
