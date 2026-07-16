#pragma once

#include <libk/concepts.hpp>

namespace libk {

template<typename Lock>
concept BasicLockable = requires(Lock& lock) {
    { lock.lock() } noexcept -> SameAs<void>;
    { lock.unlock() } noexcept -> SameAs<void>;
};

template<BasicLockable Lock>
class [[nodiscard("a temporary LockGuard releases the lock immediately")]]
    LockGuard final {
public:
    explicit LockGuard(Lock& lock) noexcept : lock_(lock) {
        lock_.lock();
    }

    LockGuard(const LockGuard&) = delete;
    auto operator=(const LockGuard&) -> LockGuard& = delete;
    LockGuard(LockGuard&&) = delete;
    auto operator=(LockGuard&&) -> LockGuard& = delete;

    ~LockGuard() noexcept {
        lock_.unlock();
    }

private:
    Lock& lock_;
};

template<BasicLockable Lock>
LockGuard(Lock&) -> LockGuard<Lock>;

} // namespace libk
