#pragma once

#include <stdint.h>

#include <atomic>

namespace libk {

// Fair, allocation-free mutual exclusion for short non-sleeping sections.
// IRQ and preemption discipline are deliberately left to kernel-level guards.
class ticket_lock final {
public:
    using ticket_type = uint32_t;

    constexpr ticket_lock() noexcept = default;

    ticket_lock(const ticket_lock&) = delete;
    auto operator=(const ticket_lock&) -> ticket_lock& = delete;
    ticket_lock(ticket_lock&&) = delete;
    auto operator=(ticket_lock&&) -> ticket_lock& = delete;
    ~ticket_lock() noexcept = default;

    void lock() noexcept {
        const ticket_type ticket =
            next_ticket_.fetch_add(ticket_type{1}, std::memory_order_relaxed);
        while (serving_ticket_.load(std::memory_order_relaxed) != ticket) {
            pause();
        }
        // The final relaxed load observes the preceding owner's release store;
        // acquire once after the wait instead of fencing every poll.
        std::atomic_thread_fence(std::memory_order_acquire);
    }

    [[nodiscard]] auto try_lock() noexcept -> bool {
        const ticket_type serving =
            serving_ticket_.load(std::memory_order_acquire);
        ticket_type expected = next_ticket_.load(std::memory_order_relaxed);
        if (expected != serving) {
            return false;
        }

        // The acquire load of serving_ticket_ observes the preceding unlock.
        // The CAS only reserves the next ticket and therefore stays relaxed.
        return next_ticket_.compare_exchange_strong(expected, expected + ticket_type{1}, std::memory_order_relaxed, std::memory_order_relaxed);
    }

    void unlock() noexcept {
        // Only the lock owner advances serving_ticket_, so a release store is
        // sufficient and avoids an unnecessary read-modify-write instruction.
        const ticket_type next =
            serving_ticket_.load(std::memory_order_relaxed) + ticket_type{1};
        serving_ticket_.store(next, std::memory_order_release);
    }

private:
    static void pause() noexcept {
#if defined(__i386__) || defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    asm volatile("yield" ::: "memory");
#else
    // The atomic load remains the synchronization operation. This fence only
    // prevents an unsupported target from turning the empty wait body into an
    // overly aggressive compiler loop.
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
    }

    std::atomic<ticket_type> next_ticket_{};
    std::atomic<ticket_type> serving_ticket_{};
};

static_assert(std::atomic<ticket_lock::ticket_type>::is_always_lock_free);

} // namespace libk
