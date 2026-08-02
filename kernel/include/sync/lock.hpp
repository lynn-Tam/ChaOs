#pragma once

#include <libk/sync/ticket_spin_lock.hpp>
#include <libk/utility.hpp>
#include <sync/trace.hpp>

namespace kernel::sync {

struct LockAccess;

template<LockClass Class,
    SameClassPolicy SameClass = SameClassPolicy::Forbidden>
class SpinLock final {
public:
    constexpr SpinLock() noexcept = default;

    SpinLock(const SpinLock&) = delete;
    auto operator=(const SpinLock&) -> SpinLock& = delete;
    SpinLock(SpinLock&&) = delete;
    auto operator=(SpinLock&&) -> SpinLock& = delete;
    ~SpinLock() noexcept = default;

    static constexpr LockClass lock_class = Class;
    static constexpr SameClassPolicy same_class = SameClass;

private:
    friend struct LockAccess;

    struct EmptyOwner final {};
    struct DebugOwner final {
        libk::Atomic<u64> word{};
    };
    using Owner = libk::conditional_t<lock_trace, DebugOwner, EmptyOwner>;

    libk::TicketSpinLock raw_{};
    [[no_unique_address]] Owner owner_{};
};

template<typename T>
concept KernelLock = requires {
    { T::lock_class };
    { T::same_class };
};

struct LockAccess final {
    // This token is deliberately narrower than IrqLockToken. It is for an
    // observer-internal IRQ-off notification that may try a lock exactly once
    // without entering lock graph, flight, or profile accounting. The caller
    // must already have interrupts disabled; it owns no IRQ state and cannot
    // be used as a general lock guard.
    template<KernelLock Lock>
    class [[nodiscard("an ignored observer try-lock token leaks the lock")]]
    ObserverTryLockToken final {
    public:
        explicit ObserverTryLockToken(Lock& lock) noexcept
            : lock_(&lock), owns_(lock.raw_.try_lock()) {}

        ObserverTryLockToken(const ObserverTryLockToken&) = delete;
        auto operator=(const ObserverTryLockToken&)
            -> ObserverTryLockToken& = delete;
        ObserverTryLockToken(ObserverTryLockToken&& other) noexcept
            : lock_(libk::exchange(other.lock_, nullptr)),
              owns_(libk::exchange(other.owns_, false)) {}
        auto operator=(ObserverTryLockToken&&)
            -> ObserverTryLockToken& = delete;

        ~ObserverTryLockToken() noexcept {
            if (owns_) {
                lock_->raw_.unlock();
            }
        }

        [[nodiscard]] auto owns_lock() const noexcept -> bool {
            return owns_;
        }

    private:
        Lock* lock_{};
        bool owns_{};
    };

    template<LockClass Class, SameClassPolicy SameClass>
    [[nodiscard]] static auto ref(SpinLock<Class, SameClass>& lock) noexcept
        -> LockRef {
        libk::Atomic<u64>* owner{};
        if constexpr (lock_trace) {
            owner = &lock.owner_.word;
        }
        return LockRef{&lock, owner, Class, SameClass};
    }

    template<LockClass Class, SameClassPolicy SameClass>
    [[nodiscard]] static auto acquire(
        SpinLock<Class, SameClass>& lock, LockSite site) noexcept
        -> LockCookie {
        if constexpr (!lock_runtime) {
            lock.raw_.lock();
            return {};
        } else {
            const LockRef identity = ref(lock);
            LockCookie cookie = before_acquire(identity, site);
            if constexpr (lock_trace) {
                struct Observer final {
                    LockRef lock;
                    LockSite site;
                    LockCookie* cookie;
                    u32 polls{};

                    void operator()(u32 ticket, u32 serving) noexcept {
                        ++polls;
                        cookie->contended = true;
                        if (polls == 1 || (polls & 0x3ffU) == 0) {
                            on_spin(lock, site, ticket, serving, polls);
                        }
                    }
                } observer{identity, site, &cookie};
                lock.raw_.lock(observer);
            } else {
                lock.raw_.lock();
            }
            return after_acquire(identity, site, cookie);
        }
    }

    template<LockClass Class, SameClassPolicy SameClass>
    [[nodiscard]] static auto try_acquire(
        SpinLock<Class, SameClass>& lock,
        LockSite site,
        LockCookie& cookie) noexcept -> bool {
        if constexpr (!lock_runtime) {
            return lock.raw_.try_lock();
        } else {
            const LockRef identity = ref(lock);
            cookie = before_try(identity, site);
            if (!lock.raw_.try_lock()) {
                cancel_try(cookie);
                cookie = {};
                return false;
            }
            cookie = after_try(identity, site, cookie);
            return true;
        }
    }

#if MYOS_LOCK_PROBE
    //Confirmatory experiment.
    // Only the stable wait-cycle probe bypasses structural graph insertion;
    // normal builds do not contain this entry point.
    template<LockClass Class, SameClassPolicy SameClass>
    [[nodiscard]] static auto acquire_wait_probe(
        SpinLock<Class, SameClass>& lock, LockSite site) noexcept
        -> LockCookie {
        const LockRef identity = ref(lock);
        LockCookie cookie = before_wait_probe(identity, site);
        struct Observer final {
            LockRef lock;
            LockSite site;
            LockCookie* cookie;
            u32 polls{};

            void operator()(u32 ticket, u32 serving) noexcept {
                ++polls;
                cookie->contended = true;
                if (polls == 1 || (polls & 0x3ffU) == 0) {
                    on_spin(lock, site, ticket, serving, polls);
                }
            }
        } observer{identity, site, &cookie};
        lock.raw_.lock(observer);
        return after_acquire(identity, site, cookie);
    }
#endif

    template<LockClass Class, SameClassPolicy SameClass>
    static void release(
        SpinLock<Class, SameClass>& lock,
        LockSite site,
        LockCookie cookie) noexcept {
        if constexpr (lock_runtime) {
            before_release(ref(lock), site, cookie);
        }
        lock.raw_.unlock();
    }

    template<LockClass Class, SameClassPolicy SameClass>
    static void assert_held(
        SpinLock<Class, SameClass>& lock,
        LockSite site = LockSite::current()) noexcept {
        if constexpr (lock_verify) {
            sync::assert_held(ref(lock), site);
        }
    }
};

#if MYOS_LOCK_DIAG == 0 && MYOS_CONCURRENCY_DIAG == 0
static_assert(sizeof(SpinLock<LockClass::Pmm>)
    == sizeof(libk::TicketSpinLock));
static_assert(alignof(SpinLock<LockClass::Pmm>)
    == alignof(libk::TicketSpinLock));
#endif

} // namespace kernel::sync
