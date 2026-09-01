#include <irq/irq.hpp>

#include <arch/riscv64/cpu/csr.hpp>
#include <arch/uart.hpp>
#include <libk/limits.hpp>
#include <libk/sync/atomic.hpp>
#include <mm/virtual_layout.hpp>
#include <sync/irq_lock_guard.hpp>

namespace kernel::irq {

namespace {

constexpr usize max_sources = 64;
kernel::sync::SpinLock<kernel::sync::LockClass::IrqRegistry>
    registry_lock{};
Irq* registry[max_sources]{};
arch::riscv64::Plic plic{
    kernel::mm::layout::DirectMapBegin + arch::riscv64::virt_plic_base};
libk::Atomic<bool> platform_ready{};

[[nodiscard]] auto platform_source(u32 source) noexcept -> bool {
    return source == arch::riscv64::virt_uart_irq;
}

[[nodiscard]] auto platform_enabled(u32 source) noexcept -> bool {
    return platform_source(source)
        && platform_ready.load<libk::MemoryOrder::Acquire>();
}

/* Every registry operation is performed with registry_lock held.  Callers
 * that also need the Irq state lock acquire them in that order: registry,
 * then Irq.  Dispatch therefore keeps the raw pointer inside the same
 * lifetime boundary until observe() returns. */
[[nodiscard]] auto register_source_locked(Irq& irq) noexcept -> bool {
    if (irq.source().id() >= max_sources) {
        return false;
    }
    Irq*& entry = registry[irq.source().id()];
    if (entry != nullptr && entry != &irq) {
        return false;
    }
    entry = &irq;
    return true;
}

void unregister_source_locked(Irq& irq) noexcept {
    if (irq.source().id() >= max_sources) {
        return;
    }
    if (registry[irq.source().id()] == &irq) {
        registry[irq.source().id()] = nullptr;
    }
}

} // namespace

void initialize_platform() noexcept {
    // This is deliberately after install_local_entry(): before that point a
    // PLIC interrupt would have no valid S-mode trap target.  Construction
    // and builtin tests therefore exercise only the Irq state machine.
    {
        kernel::sync::IrqLockGuard guard{registry_lock};
        Irq* const target = registry[arch::riscv64::virt_uart_irq];
        if (target != nullptr) {
            kernel::sync::IrqLockGuard irq_guard{target->lock_};
            plic.configure(target->source_.id());
            if (target->state_ == State::BoundIdle) {
                plic.unmask(target->source_.id());
            } else {
                /* A retained Pending obligation remains masked until its
                 * exact acknowledgement. */
                plic.mask(target->source_.id());
            }
        } else {
            plic.configure(arch::riscv64::virt_uart_irq);
            /* No published Irq owns the source yet.  Keep the unbound source
             * masked until a committed idle bind explicitly unmasks it. */
            plic.mask(arch::riscv64::virt_uart_irq);
        }
        /* Publish readiness before releasing the registry transaction.  A
         * concurrent bind must observe the fully initialized PLIC edge, not
         * a transient state in which the source is published but still
         * masked by the bootstrap setup. */
        platform_ready.store<libk::MemoryOrder::Release>(true);
    }
    arch::riscv64::Sie::enable_external();
}

Irq::Irq(SourceToken source) noexcept
    : source_(source),
      source_link_(ipc::NotificationSource::bind<
          Irq,
          &Irq::notification_closed>(*this)) {
    KASSERT(source_);
}

Irq::~Irq() noexcept {
    KASSERT(state_ == State::UnboundIdle || state_ == State::UnboundPending
            || state_ == State::Closed);
    KASSERT(notification_ == nullptr && !source_link_.attached());
    KASSERT(!cleanup_);
}

auto Irq::state() const noexcept -> State {
    kernel::sync::IrqLockGuard guard{lock_};
    return state_;
}

auto Irq::delivery_sequence() const noexcept -> u64 {
    kernel::sync::IrqLockGuard guard{lock_};
    return sequence_;
}

auto Irq::bind(ipc::Notification& notification, u64 badge) noexcept
    -> libk::Expected<void, Error> {
    if (badge == 0) {
        return libk::unexpected(Error::InvalidState);
    }
    bool pending = false;
    u64 next_generation = 0;
    {
        kernel::sync::IrqLockGuard registry_guard{registry_lock};
        kernel::sync::IrqLockGuard irq_guard{lock_};
        if ((state_ != State::UnboundIdle
             && state_ != State::UnboundPending)
            || notification_ != nullptr) {
            return libk::unexpected(Error::Busy);
        }
        if (generation_ == libk::numeric_limits<u64>::max()) {
            return libk::unexpected(Error::Closed);
        }
        pending = state_ == State::UnboundPending;
        next_generation = generation_ + 1;
    }

    /* Bind the notification before publishing either the registry entry or
     * the new generation.  Any failure below leaves the previous unbound
     * state intact and the relation is rolled back through source_link_. */
    auto attached = notification.bind(source_link_, badge);
    if (!attached) {
        return libk::unexpected(Error::Busy);
    }
    if (!source_link_.attached()) {
        source_link_.reset();
        return libk::unexpected(Error::Busy);
    }
    bool rollback = false;
    bool republish = false;
    {
        /* Registry is retained through the publication and, for a retained
         * Pending, through the reassert signal.  This keeps dispatch from
         * entering the newly published object until the complete bind
         * transaction has committed, while the Irq lock never surrounds the
         * foreign Notification operation itself. */
        kernel::sync::IrqLockGuard registry_guard{registry_lock};
        {
            kernel::sync::IrqLockGuard irq_guard{lock_};
            if ((state_ != State::UnboundIdle
                 && state_ != State::UnboundPending)
                || notification_ != nullptr || !source_link_.attached()) {
                rollback = true;
            } else if (!register_source_locked(*this)) {
                rollback = true;
            } else {
                notification_ = &notification;
                generation_ = next_generation;
                state_ = pending ? State::BoundPending : State::BoundIdle;
                if (platform_enabled(source_.id())) {
                    if (pending) {
                        /* The source was masked when it became unbound; keep
                         * that invariant explicit across the new relation. */
                        plic.mask(source_.id());
                    } else {
                        plic.unmask(source_.id());
                    }
                }
                republish = pending;
            }
        }
        if (!rollback && republish) {
            /* NotificationSource::signal retains its own relation lease and
             * is intentionally called with the Irq lock released. */
            static_cast<void>(source_link_.signal());
        }
    }
    if (rollback) {
        source_link_.reset();
        return libk::unexpected(Error::Busy);
    }
    return libk::expected();
}

auto Irq::unbind() noexcept -> bool {
    ipc::Notification* notification{};
    {
        kernel::sync::IrqLockGuard registry_guard{registry_lock};
        kernel::sync::IrqLockGuard irq_guard{lock_};
        if (state_ == State::Closed || state_ == State::Closing) {
            return false;
        }
        notification = notification_;

        /* Registry is the outer lifetime boundary.  Remove the dispatch
         * publication before converting the relation to an unbound state,
         * and keep the hardware transition under the Irq lock. */
        unregister_source_locked(*this);
        if (platform_enabled(source_.id())) {
            plic.mask(source_.id());
        }
        notification_ = nullptr;
        state_ = observed_since_ack_ ? State::UnboundPending
                                     : State::UnboundIdle;
    }

    /* NotificationSource::reset() may invoke the foreign Notification
     * callback, so it remains outside both synchronization boundaries. */
    if (notification != nullptr) {
        source_link_.reset();
    }
    return notification != nullptr;
}

auto Irq::delivery() const noexcept
    -> libk::Expected<Delivery, Error> {
    kernel::sync::IrqLockGuard guard{lock_};
    if (state_ == State::Closed || state_ == State::Closing) {
        return libk::unexpected(Error::Closed);
    }
    if (state_ != State::BoundPending || !observed_since_ack_
        || sequence_ == 0 || generation_ == 0) {
        return libk::unexpected(Error::InvalidState);
    }
    return libk::expected(Delivery{sequence_, generation_});
}

void Irq::notification_closed() noexcept {
    {
        kernel::sync::IrqLockGuard registry_guard{registry_lock};
        kernel::sync::IrqLockGuard irq_guard{lock_};
        /* A close callback for an old relation can race a new bind after the
         * old source was detached.  If the source is attached again, the
         * callback has no authority over that new relation. */
        if (source_link_.attached()) {
            return;
        }
        /* Notification::detach_source has already removed the reverse edge;
         * keep the Irq visibly bound until dispatch publication and hardware
         * delivery are detached.  This also serializes a rebind against the
         * old registry entry without invoking a foreign callback under the
         * Irq lock. */
        unregister_source_locked(*this);
        if (platform_enabled(source_.id())) {
            plic.mask(source_.id());
        }
        notification_ = nullptr;
        if (state_ == State::BoundPending) {
            state_ = State::UnboundPending;
        } else if (state_ == State::BoundIdle) {
            state_ = State::UnboundIdle;
        }
    }
}

auto Irq::observe_locked() noexcept -> libk::Expected<Delivery, Error> {
    ipc::NotificationSource* link{};
    Delivery delivery{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ == State::Closing || state_ == State::Closed) {
            return libk::unexpected(Error::Closed);
        }
        if (state_ != State::BoundIdle && state_ != State::BoundPending) {
            return libk::unexpected(Error::InvalidState);
        }
        if (sequence_ == libk::numeric_limits<u64>::max()) {
            if (platform_enabled(source_.id())) {
                plic.mask(source_.id());
            }
            state_ = State::Closing;
            return libk::unexpected(Error::Closed);
        }
        ++sequence_;
        observed_since_ack_ = true;
        state_ = State::BoundPending;
        delivery = Delivery{sequence_, generation_};
        link = &source_link_;
        if (platform_enabled(source_.id())) {
            plic.mask(source_.id());
        }
    }
    // NotificationSource performs the retained relation lease and coalesces
    // badges.  It is deliberately outside the Irq lock.
    static_cast<void>(link->signal());
    return libk::expected(delivery);
}

auto Irq::observe() noexcept -> libk::Expected<Delivery, Error> {
    libk::Expected<Delivery, Error> result =
        libk::unexpected(Error::InvalidState);
    ipc::Notification* notification{};
    bool closed = false;
    {
        kernel::sync::IrqLockGuard registry_guard{registry_lock};
        result = observe_locked();
        if (!result && state_ == State::Closing) {
            closed = close_locked(notification);
        }
    }
    if (closed) {
        finish_close(notification);
    }
    return result;
}

auto Irq::ack(u64 generation, u64 sequence) noexcept
    -> libk::Expected<void, Error> {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ == State::Closed || state_ == State::Closing) {
            return libk::unexpected(Error::Closed);
        }
        if (state_ != State::BoundPending || !observed_since_ack_) {
            return libk::unexpected(Error::InvalidState);
        }
        if (generation != generation_) {
            return libk::unexpected(Error::StaleSequence);
        }
        if (sequence == 0 || sequence > sequence_) {
            return libk::unexpected(Error::BadSequence);
        }
        if (sequence < sequence_) {
            return libk::unexpected(Error::StaleSequence);
        }
        observed_since_ack_ = false;
        state_ = State::BoundIdle;
        if (platform_enabled(source_.id())) {
            /* Keep this MMIO edge in the same Irq critical section as the
             * state release.  A concurrent detach cannot subsequently be
             * overtaken by a delayed unmask. */
            plic.unmask(source_.id());
        }
    }
    return libk::expected();
}

void Irq::dispatch(u32 source) noexcept {
    if (source >= max_sources) {
        return;
    }
    {
        kernel::sync::IrqLockGuard registry_guard{registry_lock};
        Irq* const target = registry[source];
        if (target != nullptr) {
            /* The registry lock is the lifetime boundary for this raw
             * pointer.  Sequence exhaustion only unpublishes the source and
             * enters Closing here; relation detach and cleanup wait for a
             * later close/retire call that owns a live object reference. */
            const auto observed = target->observe_locked();
            if (!observed) {
                kernel::sync::IrqLockGuard irq_guard{target->lock_};
                if (target->state_ == State::Closing) {
                    unregister_source_locked(*target);
                    if (platform_enabled(target->source_.id())) {
                        plic.mask(target->source_.id());
                    }
                }
            }
        }
    }
}

auto Irq::close_locked(ipc::Notification*& notification) noexcept -> bool {
    kernel::sync::IrqLockGuard irq_guard{lock_};
    notification = nullptr;
    if (state_ == State::Closed) {
        return false;
    }
    state_ = State::Closing;
    notification = notification_;
    notification_ = nullptr;
    unregister_source_locked(*this);
    if (platform_enabled(source_.id())) {
        plic.mask(source_.id());
    }
    state_ = State::Closed;
    return true;
}

void Irq::finish_close(ipc::Notification* notification) noexcept {
    /* Detaching the foreign Notification and completing object cleanup both
     * remain outside the Irq lock (and outside registry_lock). */
    if (notification != nullptr || source_link_.attached()) {
        source_link_.reset();
    }
    object::ObjectCleanup cleanup{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        cleanup = libk::move(cleanup_);
    }
    if (cleanup) {
        cleanup.complete();
    }
}

auto Irq::close() noexcept -> bool {
    ipc::Notification* notification{};
    {
        kernel::sync::IrqLockGuard registry_guard{registry_lock};
        static_cast<void>(close_locked(notification));
    }
    /* close() is called through a live object reference (and by retire while
     * its pool pin is held), so it also drains a cleanup installed after an
     * earlier sequence-exhaustion close. */
    finish_close(notification);
    return true;
}

void Irq::retire(object::ObjectCleanup&& cleanup) noexcept {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        KASSERT(!cleanup_);
        cleanup_ = libk::move(cleanup);
    }
    static_cast<void>(close());
}

} // namespace kernel::irq
