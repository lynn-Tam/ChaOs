#pragma once

#include <stdint.h>

#include <libk/sync/atomic.hpp>

namespace libk {

namespace spin_lock_detail {

inline void wait_hint() noexcept {
#if defined(__i386__) || defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    asm volatile("yield" ::: "memory");
#else
    // The atomic load remains the synchronization operation. This fence only
    // prevents an unsupported target from turning the empty wait body into an
    // overly aggressive compiler loop.
    atomic_signal_fence<MemoryOrder::SeqCst>();
#endif
}

} // namespace spin_lock_detail

// Fair, allocation-free mutual exclusion for short non-sleeping sections.
// IRQ and preemption discipline are deliberately left to kernel-level guards.
class TicketSpinLock final {
public:
    using ticket_type = uint32_t;

    constexpr TicketSpinLock() noexcept = default;

    TicketSpinLock(const TicketSpinLock&) = delete;
    auto operator=(const TicketSpinLock&) -> TicketSpinLock& = delete;
    TicketSpinLock(TicketSpinLock&&) = delete;
    auto operator=(TicketSpinLock&&) -> TicketSpinLock& = delete;
    ~TicketSpinLock() noexcept = default;

    void lock() noexcept {
        const ticket_type ticket =
            next_ticket_.fetch_add<MemoryOrder::Relaxed>(ticket_type{1});
        while (serving_ticket_.load<MemoryOrder::Relaxed>() != ticket) {
            spin_lock_detail::wait_hint();
        }
        // The final relaxed load observes the preceding owner's release store;
        // acquire once after the wait instead of fencing every poll.
        atomic_thread_fence<MemoryOrder::Acquire>();
    }

    [[nodiscard]] auto try_lock() noexcept -> bool {
        const ticket_type serving =
            serving_ticket_.load<MemoryOrder::Acquire>();
        ticket_type expected = next_ticket_.load<MemoryOrder::Relaxed>();
        if (expected != serving) {
            return false;
        }

        // The acquire load of serving_ticket_ observes the preceding unlock.
        // The CAS only reserves the next ticket and therefore stays relaxed.
        return next_ticket_.compare_exchange_strong<
            MemoryOrder::Relaxed,
            MemoryOrder::Relaxed>(expected, expected + ticket_type{1});
    }

    void unlock() noexcept {
        // Only the lock owner advances serving_ticket_, so a release store is
        // sufficient and avoids an unnecessary read-modify-write instruction.
        const ticket_type next =
            serving_ticket_.load<MemoryOrder::Relaxed>() + ticket_type{1};
        serving_ticket_.store<MemoryOrder::Release>(next);
    }

private:
    Atomic<ticket_type> next_ticket_{};
    Atomic<ticket_type> serving_ticket_{};
};

static_assert(AtomicHasScalarLayout<TicketSpinLock::ticket_type>);

} // namespace libk
