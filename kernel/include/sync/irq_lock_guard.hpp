#pragma once

#include <arch/interrupt.hpp>
#include <libk/utility.hpp>
#include <sync/lock.hpp>

namespace kernel::sync {

class IrqToken final {
public:
    explicit IrqToken(
        LockSite site = LockSite::current()) noexcept
        : interrupts_(arch::disable_interrupts()), active_(true) {
        if constexpr (lock_verify) {
            cookie_ = irq_disabled(site);
        }
    }

    IrqToken(const IrqToken&) = delete;
    auto operator=(const IrqToken&) -> IrqToken& = delete;

    IrqToken(IrqToken&& other) noexcept
        : interrupts_(other.interrupts_),
          active_(libk::exchange(other.active_, false)),
          cookie_(other.cookie_) {
        other.cookie_ = {};
    }
    auto operator=(IrqToken&&) -> IrqToken& = delete;

    ~IrqToken() noexcept { restore(); }

    void restore() noexcept {
        if (!active_) {
            return;
        }
        if constexpr (lock_verify) {
            irq_restoring(cookie_);
        }
        arch::restore_interrupts(interrupts_);
        active_ = false;
    }

    [[nodiscard]] auto active() const noexcept -> bool { return active_; }

private:
    arch::InterruptState interrupts_;
    bool active_{};
    IrqCookie cookie_{};
};

template<KernelLock Lock>
class [[nodiscard("a temporary IrqLockGuard releases the lock immediately")]]
    IrqLockGuard final {
public:
    explicit IrqLockGuard(
        Lock& lock,
        LockSite site = LockSite::current()) noexcept
        : irq_(site), lock_(lock), site_(site),
          cookie_(LockAccess::acquire(lock_, site_)) {}

    IrqLockGuard(const IrqLockGuard&) = delete;
    auto operator=(const IrqLockGuard&) -> IrqLockGuard& = delete;
    IrqLockGuard(IrqLockGuard&&) = delete;
    auto operator=(IrqLockGuard&&) -> IrqLockGuard& = delete;

    ~IrqLockGuard() noexcept {
        LockAccess::release(lock_, site_, cookie_);
    }

private:
    // Destruction is reverse declaration order: lock releases before irq_.
    IrqToken irq_;
    Lock& lock_;
    LockSite site_;
    LockCookie cookie_{};
};

template<KernelLock Lock>
IrqLockGuard(Lock&, LockSite = LockSite::current()) -> IrqLockGuard<Lock>;

struct TryLock final {};
inline constexpr TryLock try_lock{};

template<KernelLock Lock>
class [[nodiscard("an ignored IrqLockToken releases ownership immediately")]]
    IrqLockToken final {
public:
    explicit IrqLockToken(
        Lock& lock,
        LockSite site = LockSite::current()) noexcept
        : irq_(site), lock_(&lock), site_(site),
          cookie_(LockAccess::acquire(lock, site)) {}

    IrqLockToken(
        Lock& lock,
        TryLock,
        LockSite site = LockSite::current()) noexcept
        : irq_(site), site_(site) {
        if (LockAccess::try_acquire(lock, site, cookie_)) {
            lock_ = &lock;
        } else {
            irq_.restore();
        }
    }

    IrqLockToken(const IrqLockToken&) = delete;
    auto operator=(const IrqLockToken&) -> IrqLockToken& = delete;

    IrqLockToken(IrqLockToken&& other) noexcept
        : irq_(libk::move(other.irq_)),
          lock_(libk::exchange(other.lock_, nullptr)),
          site_(other.site_), cookie_(other.cookie_) {
        other.cookie_ = {};
    }
    auto operator=(IrqLockToken&&) -> IrqLockToken& = delete;

    ~IrqLockToken() noexcept { unlock(); }

    [[nodiscard]] auto owns_lock() const noexcept -> bool {
        return lock_ != nullptr;
    }

    void unlock() noexcept {
        if (lock_ == nullptr) {
            return;
        }
        LockAccess::release(*lock_, site_, cookie_);
        lock_ = nullptr;
        cookie_ = {};
    }

    void restore() noexcept {
        unlock();
        irq_.restore();
    }

private:
    IrqToken irq_;
    Lock* lock_{};
    LockSite site_{};
    LockCookie cookie_{};
};

template<KernelLock Lock>
IrqLockToken(Lock&, LockSite = LockSite::current()) -> IrqLockToken<Lock>;

template<KernelLock Lock>
IrqLockToken(Lock&, TryLock, LockSite = LockSite::current())
    -> IrqLockToken<Lock>;

template<KernelLock Lock>
class [[nodiscard("a temporary lock pair releases both locks immediately")]]
    OrderedIrqLockPair final {
public:
    OrderedIrqLockPair(
        Lock& left,
        Lock& right,
        LockSite site = LockSite::current()) noexcept
        : irq_(site), site_(site) {
        first_ = &left;
        second_ = &right;
        if (reinterpret_cast<usize>(first_) > reinterpret_cast<usize>(second_)) {
            libk::swap(first_, second_);
        }
        first_cookie_ = LockAccess::acquire(*first_, site_);
        if (second_ != first_) {
            second_cookie_ = LockAccess::acquire(*second_, site_);
        } else {
            second_ = nullptr;
        }
    }

    OrderedIrqLockPair(const OrderedIrqLockPair&) = delete;
    auto operator=(const OrderedIrqLockPair&)
        -> OrderedIrqLockPair& = delete;
    OrderedIrqLockPair(OrderedIrqLockPair&&) = delete;
    auto operator=(OrderedIrqLockPair&&)
        -> OrderedIrqLockPair& = delete;

    ~OrderedIrqLockPair() noexcept { release(); }

    void release() noexcept {
        if (first_ == nullptr) {
            return;
        }
        if (second_ != nullptr) {
            LockAccess::release(*second_, site_, second_cookie_);
        }
        LockAccess::release(*first_, site_, first_cookie_);
        second_ = nullptr;
        first_ = nullptr;
        irq_.restore();
    }

private:
    IrqToken irq_;
    Lock* first_{};
    Lock* second_{};
    LockSite site_{};
    LockCookie first_cookie_{};
    LockCookie second_cookie_{};
};

template<KernelLock Lock>
OrderedIrqLockPair(Lock&, Lock&, LockSite = LockSite::current())
    -> OrderedIrqLockPair<Lock>;

} // namespace kernel::sync
