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

[[nodiscard]] auto register_source(Irq& irq) noexcept -> bool {
    if (irq.source().id() >= max_sources) {
        return false;
    }
    kernel::sync::IrqLockGuard guard{registry_lock};
    Irq*& entry = registry[irq.source().id()];
    if (entry != nullptr && entry != &irq) {
        return false;
    }
    entry = &irq;
    return true;
}

void unregister_source(Irq& irq) noexcept {
    if (irq.source().id() >= max_sources) {
        return;
    }
    kernel::sync::IrqLockGuard guard{registry_lock};
    if (registry[irq.source().id()] == &irq) {
        registry[irq.source().id()] = nullptr;
    }
}

} // namespace

void initialize_platform() noexcept {
    // This is deliberately after install_local_entry(): before that point a
    // PLIC interrupt would have no valid S-mode trap target.  Construction
    // and builtin tests therefore exercise only the Irq state machine.
    plic.initialize(arch::riscv64::virt_uart_irq);
    platform_ready.store<libk::MemoryOrder::Release>(true);
    {
        kernel::sync::IrqLockGuard guard{registry_lock};
        if (registry[arch::riscv64::virt_uart_irq] != nullptr) {
            plic.unmask(arch::riscv64::virt_uart_irq);
        }
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
    KASSERT(state_ == State::MaskedUnbound || state_ == State::Closed);
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
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != State::MaskedUnbound || notification_ != nullptr) {
            return libk::unexpected(Error::Busy);
        }
    }
    auto attached = notification.bind(source_link_, badge);
    if (!attached) {
        return libk::unexpected(Error::Busy);
    }
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ != State::MaskedUnbound || notification_ != nullptr) {
            source_link_.reset();
            return libk::unexpected(Error::Busy);
        }
        notification_ = &notification;
        state_ = State::Armed;
    }
    if (!register_source(*this)) {
        {
            kernel::sync::IrqLockGuard guard{lock_};
            notification_ = nullptr;
            state_ = State::MaskedUnbound;
        }
        source_link_.reset();
        return libk::unexpected(Error::Busy);
    }
    if (platform_enabled(source_.id())) {
        plic.initialize(source_.id());
        plic.unmask(source_.id());
    }
    return libk::expected();
}

auto Irq::unbind() noexcept -> bool {
    ipc::Notification* notification{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        notification = notification_;
        notification_ = nullptr;
        if (state_ != State::Closed) {
            state_ = State::MaskedUnbound;
        }
    }
    unregister_source(*this);
    if (platform_enabled(source_.id())) {
        plic.mask(source_.id());
    }
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
    if (state_ != State::Pending || !observed_since_ack_
        || sequence_ == 0 || generation_ == 0) {
        return libk::unexpected(Error::InvalidState);
    }
    return libk::expected(Delivery{sequence_, generation_});
}

void Irq::notification_closed() noexcept {
    {
        kernel::sync::IrqLockGuard guard{lock_};
        notification_ = nullptr;
        if (state_ != State::Closed) {
            state_ = State::MaskedUnbound;
        }
    }
    unregister_source(*this);
    if (platform_enabled(source_.id())) {
        plic.mask(source_.id());
    }
}

auto Irq::observe() noexcept -> libk::Expected<Delivery, Error> {
    ipc::NotificationSource* link{};
    Delivery delivery{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ == State::Closing || state_ == State::Closed) {
            return libk::unexpected(Error::Closed);
        }
        if (state_ == State::MaskedUnbound) {
            return libk::unexpected(Error::InvalidState);
        }
        if (sequence_ == libk::numeric_limits<u64>::max()
            || generation_ == libk::numeric_limits<u64>::max()) {
            state_ = State::Closing;
            return libk::unexpected(Error::Closed);
        }
        ++sequence_;
        observed_since_ack_ = true;
        state_ = State::Pending;
        delivery = Delivery{sequence_, generation_};
        link = &source_link_;
    }
    if (platform_enabled(source_.id())) {
        plic.mask(source_.id());
    }
    // NotificationSource performs the retained relation lease and coalesces
    // badges.  It is deliberately outside the Irq lock.
    static_cast<void>(link->signal());
    return libk::expected(delivery);
}

auto Irq::ack(u64 sequence) noexcept
    -> libk::Expected<void, Error> {
    kernel::sync::IrqLockGuard guard{lock_};
    if (state_ == State::Closed || state_ == State::Closing) {
        return libk::unexpected(Error::Closed);
    }
    if (state_ != State::Pending || !observed_since_ack_) {
        return libk::unexpected(Error::InvalidState);
    }
    if (sequence == 0 || sequence > sequence_) {
        return libk::unexpected(Error::BadSequence);
    }
    if (sequence < sequence_) {
        return libk::unexpected(Error::StaleSequence);
    }
    observed_since_ack_ = false;
    state_ = notification_ != nullptr ? State::Armed : State::MaskedUnbound;
    if (platform_enabled(source_.id()) && notification_ != nullptr) {
        plic.unmask(source_.id());
    }
    return libk::expected();
}

void Irq::dispatch(u32 source) noexcept {
    if (source >= max_sources) {
        return;
    }
    kernel::sync::IrqLockGuard guard{registry_lock};
    Irq* const target = registry[source];
    if (target != nullptr) {
        static_cast<void>(target->observe());
    }
}

auto Irq::close() noexcept -> bool {
    ipc::Notification* notification{};
    {
        kernel::sync::IrqLockGuard guard{lock_};
        if (state_ == State::Closed) {
            return true;
        }
        state_ = State::Closing;
        notification = notification_;
        notification_ = nullptr;
    }
    unregister_source(*this);
    if (platform_enabled(source_.id())) {
        plic.mask(source_.id());
    }
    if (notification != nullptr) {
        source_link_.reset();
    }
    {
        kernel::sync::IrqLockGuard guard{lock_};
        state_ = State::Closed;
    }
    if (cleanup_) {
        cleanup_.complete();
    }
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
