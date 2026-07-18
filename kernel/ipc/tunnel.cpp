#include <object/tunnel_pool.hpp>

#include <core/debug.hpp>
#include <cpu/cpu_registry.hpp>
#include <execution/vproc.hpp>
#include <libk/limits.hpp>
#include <libk/scope_guard.hpp>
#include <libk/utility.hpp>
#include <sched/binding.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::ipc {

const cap::GrantAttachmentOps Tunnel::authority_ops_{
    .invalidate = &Tunnel::invalidate,
    .released = &Tunnel::released,
};

Tunnel::Tunnel(
    CpuRegistry& cpus,
    object::ObjectHold<Vproc>&& target,
    usize slot,
    usize tag) noexcept
    : cpus_(&cpus),
      target_(&target.get()),
      target_hold_(libk::move(target)),
      slot_(slot),
      tag_(tag),
      connect_authority_(*this, authority_ops_),
      source_link_(*this),
      target_link_(*this) {}

Tunnel::~Tunnel() noexcept {
    KASSERT(state_ == State::Constructing || state_ == State::Closed);
    KASSERT(!source_link_.hook.is_linked()
        && !target_link_.hook.is_linked());
    KASSERT(!connect_authority_.attachment.attached()
        && !connect_authority_.attachment.busy()
        && !connect_authority_.work);
    KASSERT(!source_hold_);
    KASSERT(state_ == State::Constructing || !target_hold_);
    KASSERT(relations_.load<libk::MemoryOrder::Acquire>() == 0);
    KASSERT(!cleanup_ && !close_draining_);
}

auto Tunnel::open() noexcept -> libk::Expected<void, TunnelError> {
    if (target_ == nullptr || slot_ >= MYOS_VPROC_MAX_INGRESS) {
        return libk::unexpected(TunnelError::InvalidSlot);
    }
    const auto generation = target_->attach_tunnel_target(
        target_link_, slot_, tag_);
    if (!generation) {
        return libk::unexpected(TunnelError::Busy);
    }
    kernel::sync::IrqLockGuard guard{lock_};
    KASSERT(state_ == State::Constructing);
    binding_generation_ = *generation;
    state_ = State::Listening;
    return libk::expected();
}

auto Tunnel::connect(
    Vproc& source,
    cap::CSpace& cspace,
    const cap::Resolved<Tunnel>& authority) noexcept
    -> libk::Expected<cap::CapHandle, TunnelError> {
    if (&authority.object() != this || &authority.source() != &cspace
        || &source == target_) {
        return libk::unexpected(TunnelError::WrongSource);
    }
    sched::Binding* const source_binding = source.binding();
    if (source_binding == nullptr) {
        return libk::unexpected(TunnelError::Busy);
    }
    auto source_ref = source_binding->target_reference();
    if (!source_ref) {
        return libk::unexpected(TunnelError::Busy);
    }
    auto source_hold = libk::move(source_ref).value().into_hold<Vproc>();
    if (!source_hold) {
        return libk::unexpected(TunnelError::Busy);
    }
    auto reserved = cspace.reserve_derivation();
    if (!reserved) {
        return libk::unexpected(TunnelError::ResourceExhausted);
    }
    auto transaction = libk::move(reserved).value();
    auto target = authority.reference();
    if (!target) {
        return libk::unexpected(TunnelError::Busy);
    }

    u64 claim{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ == State::Connecting) {
            return libk::unexpected(TunnelError::Busy);
        }
        if (state_ == State::Idle || state_ == State::Pending) {
            return libk::unexpected(TunnelError::AlreadyConnected);
        }
        if (state_ != State::Listening) {
            return libk::unexpected(TunnelError::Closed);
        }
        if (claim_generation_ == libk::numeric_limits<u64>::max()) {
            return libk::unexpected(TunnelError::GenerationExhausted);
        }
        ++claim_generation_;
        claim = claim_generation_;
        state_ = State::Connecting;
        retain_relation();
    }

    bool source_attached{};
    bool authority_attached{};
    cap::CapHandle installed{};
    auto rollback = libk::on_scope_exit([&]() noexcept {
        if (installed) {
            static_cast<void>(cspace.close(installed));
        }
        if (authority_attached
            && connect_authority_.attachment.attached()) {
            static_cast<void>(connect_authority_.attachment.detach());
        }
        if (source_attached) {
            source.detach_tunnel_source(source_link_);
        }
        {
            kernel::sync::IrqLockGuard guard{lock_};
            if (state_ == State::Connecting
                && claim_generation_ == claim) {
                state_ = State::Listening;
            }
        }
        release_relation();
    });

    if (!source.attach_tunnel_source(source_link_)) {
        return libk::unexpected(TunnelError::Busy);
    }
    source_attached = true;
    auto attached = authority.attach(connect_authority_.attachment);
    if (!attached) {
        return libk::unexpected(TunnelError::Busy);
    }
    authority_attached = true;

    const cap::Rights tx_rights = cap::Rights::of(
        cap::Right::Duplicate,
        cap::Right::Inspect,
        cap::Right::Signal,
        cap::Right::Close);
    const cap::TunnelConnectProof proof{*this, source, claim};
    auto child = authority.derive_tunnel_tx(
        libk::move(transaction.grant_),
        libk::move(target).value(),
        cap::GrantCeiling{tx_rights},
        proof);
    if (!child) {
        return libk::unexpected(TunnelError::ResourceExhausted);
    }
    auto published = cspace.insert(
        libk::move(transaction.slot_),
        libk::move(child).value(),
        cap::CapView{tx_rights});
    if (!published) {
        return libk::unexpected(TunnelError::ResourceExhausted);
    }
    installed = published.value();

    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != State::Connecting || claim_generation_ != claim) {
            return libk::unexpected(TunnelError::Closed);
        }
        source_ = &source;
        source_hold_ = libk::move(source_hold).value();
        state_ = State::Idle;
    }
    static_cast<void>(rollback.release());
    release_relation();
    return libk::expected(installed);
}

auto Tunnel::invoke(Vproc& caller) noexcept
    -> libk::Expected<u64, TunnelError> {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != State::Idle && state_ != State::Pending) {
            return libk::unexpected(state_ == State::Listening
                    || state_ == State::Connecting
                ? TunnelError::Busy
                : TunnelError::Closed);
        }
        if (&caller != source_) {
            return libk::unexpected(TunnelError::WrongSource);
        }
        retain_relation();
    }
    auto relation = libk::on_scope_exit(
        [this]() noexcept { release_relation(); });
    auto target_ref = target_hold_.ref();
    if (!target_ref) {
        return libk::unexpected(TunnelError::Busy);
    }
    auto target_pin = target_ref.value().pin<Vproc>();
    if (!target_pin) {
        return libk::unexpected(TunnelError::Busy);
    }

    u64 sequence{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if ((state_ != State::Idle && state_ != State::Pending)
            || &caller != source_) {
            return libk::unexpected(TunnelError::Closed);
        }
        if (signal_sequence_ == libk::numeric_limits<u64>::max()) {
            return libk::unexpected(TunnelError::GenerationExhausted);
        }
        ++signal_sequence_;
        sequence = signal_sequence_;
        state_ = State::Pending;
    }
    target_pin.value()->publish_tunnel(
        target_link_, slot_, binding_generation_, sequence, tag_, *cpus_);
    return libk::expected(sequence);
}

auto Tunnel::ack(Vproc& caller, u64 observed) noexcept
    -> libk::Expected<TunnelAck, TunnelError> {
    u64 sequence{};
    bool clear{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ == State::Idle) {
            return libk::unexpected(TunnelError::Empty);
        }
        if (state_ != State::Pending) {
            return libk::unexpected(TunnelError::Closed);
        }
        if (&caller != target_) {
            return libk::unexpected(TunnelError::WrongTarget);
        }
        if (observed > signal_sequence_ || observed == 0) {
            return libk::unexpected(TunnelError::BadSequence);
        }
        sequence = signal_sequence_;
        if (observed == signal_sequence_) {
            state_ = State::Idle;
            clear = true;
            retain_relation();
        }
    }
    if (!clear) {
        return libk::expected(TunnelAck{sequence, true});
    }
    caller.clear_tunnel(
        target_link_, slot_, binding_generation_, sequence);
    release_relation();
    return libk::expected(TunnelAck{sequence, false});
}

auto Tunnel::close_from(Vproc& caller, cap::Rights rights) noexcept
    -> libk::Expected<void, TunnelError> {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        const bool receiver = rights.contains(cap::Right::Ack)
            && &caller == target_;
        const bool sender = rights.contains(cap::Right::Signal)
            && &caller == source_;
        if (!receiver && !sender) {
            return libk::unexpected(TunnelError::WrongTarget);
        }
    }
    close();
    return libk::expected();
}

void Tunnel::close() noexcept {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ == State::Closed || state_ == State::Closing) {
            return;
        }
        state_ = State::Closing;
    }
    try_finish_close();
}

void Tunnel::try_finish_close() noexcept {
    Vproc* source{};
    Vproc* target{};
    u64 binding_generation{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != State::Closing || close_draining_
            || relations_.load<libk::MemoryOrder::Acquire>() != 0) {
            return;
        }
        close_draining_ = true;
        source = source_;
        target = target_;
        binding_generation = binding_generation_;
    }

    if (source != nullptr) {
        source->detach_tunnel_source(source_link_);
    }
    if (target != nullptr && binding_generation != 0) {
        target->detach_tunnel_target(
            target_link_, slot_, binding_generation);
    }
    if (connect_authority_.attachment.attached()) {
        static_cast<void>(connect_authority_.attachment.detach());
    }
    source_hold_.reset();
    target_hold_.reset();

    cap::GrantWork work{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        work = libk::move(connect_authority_.work);
        source_ = nullptr;
        target_ = nullptr;
        close_draining_ = false;
        state_ = State::Closed;
    }
    work.reset();
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
        try_finish_close();
        try_finish_retire();
    }
}

void Tunnel::peer_stopped(Vproc& peer) noexcept {
    static_cast<void>(peer);
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
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(&link == &connect_authority_ && !link.work);
        link.work = libk::move(work);
    }
    close();
    try_finish_retire();
}

void Tunnel::try_finish_retire() noexcept {
    object::ObjectCleanup cleanup{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != State::Closed || !cleanup_ || close_draining_
            || relations_.load<libk::MemoryOrder::Acquire>() != 0
            || connect_authority_.work) {
            return;
        }
        cleanup = libk::move(cleanup_);
    }
    cleanup.complete();
}

} // namespace kernel::ipc
