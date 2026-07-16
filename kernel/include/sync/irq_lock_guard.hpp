#pragma once

#include <arch/interrupt.hpp>
#include <libk/noncopyable.hpp>
#include <libk/sync/lock_guard.hpp>

namespace kernel::sync {

// A kernel spinlock owner must not be descheduled while holding the lock.
// Masking local interrupts also makes the same guard safe before a dispatcher
// exists and when the protected state is shared with interrupt context.
template<libk::BasicLockable Lock>
class [[nodiscard("a temporary IrqLockGuard releases the lock immediately")]]
    IrqLockGuard final : private libk::noncopyable_nonmovable {
public:
    explicit IrqLockGuard(Lock& lock) noexcept
        : interrupts_(arch::disable_interrupts()), lock_(lock) {
        lock_.lock();
    }

    ~IrqLockGuard() noexcept {
        lock_.unlock();
        arch::restore_interrupts(interrupts_);
    }

private:
    arch::InterruptState interrupts_;
    Lock& lock_;
};

template<libk::BasicLockable Lock>
IrqLockGuard(Lock&) -> IrqLockGuard<Lock>;

} // namespace kernel::sync
