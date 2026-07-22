#pragma once

#include <libk/sync/ticket_spin_lock.hpp>
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
        if constexpr (!lock_verify) {
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
        if constexpr (!lock_verify) {
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
        if constexpr (lock_verify) {
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

#if MYOS_LOCK_DIAG == 0
static_assert(sizeof(SpinLock<LockClass::Pmm>)
    == sizeof(libk::TicketSpinLock));
static_assert(alignof(SpinLock<LockClass::Pmm>)
    == alignof(libk::TicketSpinLock));
#endif

} // namespace kernel::sync
