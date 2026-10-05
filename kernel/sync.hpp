#pragma once

#include <arch/interrupt.hpp>
#include <base/types.hpp>
#include <libk/assert.hpp>
#include <libk/delegate.hpp>
#include <libk/noncopyable.hpp>
#include <libk/sync/ticket_spin_lock.hpp>
#include <utility>

namespace sync {

// CPU ownership is checked without allocating a second lock/wait model.
// All acquisitions mask IRQs and finish before a scheduler handoff.
class Spin final {
public:
    void lock() noexcept;
    [[nodiscard]] auto try_lock() noexcept -> bool;
    void unlock() noexcept;
    [[nodiscard]] auto held() const noexcept -> bool;
private:
    libk::TicketSpinLock mutex_{};
    libk::Atomic<usize> owner_{};
};

void assert_unlocked() noexcept;

class Irq final {
public:
    Irq() noexcept : state_(arch::disable_interrupts()) {}
    Irq(const Irq&) = delete;
    auto operator=(const Irq&) -> Irq& = delete;
    Irq(Irq&& other) noexcept
        : state_(other.state_), active_(std::exchange(other.active_, false)) {}
    auto operator=(Irq&&) -> Irq& = delete;
    ~Irq() noexcept { restore(); }
    void restore() noexcept {
        if (std::exchange(active_, false)) arch::restore_interrupts(state_);
    }
    [[nodiscard]] auto active() const noexcept -> bool { return active_; }
private:
    arch::InterruptState state_;
    bool active_{true};
};

inline constexpr struct Try {} try_lock;

// unlock() leaves IRQs masked; restore() releases both obligations. Moving
// transfers ownership, including the original IRQ state, exactly once.
template<class M>
class [[nodiscard]] Lock final {
public:
    explicit Lock(M& m) noexcept : mutex_(&m) { m.lock(); }
    Lock(M& m, Try) noexcept : mutex_(m.try_lock() ? &m : nullptr) {}
    Lock(const Lock&) = delete;
    auto operator=(const Lock&) -> Lock& = delete;
    Lock(Lock&& other) noexcept
        : irq_(std::move(other.irq_)), mutex_(std::exchange(other.mutex_, nullptr)) {}
    auto operator=(Lock&&) -> Lock& = delete;
    ~Lock() noexcept { unlock(); }
    [[nodiscard]] auto owns_lock() const noexcept -> bool { return mutex_ != nullptr; }
    void unlock() noexcept {
        if (auto* m = std::exchange(mutex_, nullptr)) m->unlock();
    }
    void restore() noexcept { unlock(); irq_.restore(); }
private:
    Irq irq_;
    M* mutex_{};
};

// IRQs are already masked at the diagnostic observer boundary.
template<class M>
class [[nodiscard]] TryLock final {
public:
    explicit TryLock(M& m) noexcept : mutex_(m.try_lock() ? &m : nullptr) {}
    TryLock(const TryLock&) = delete;
    auto operator=(const TryLock&) -> TryLock& = delete;
    ~TryLock() noexcept { if (mutex_) mutex_->unlock(); }
    [[nodiscard]] auto owns_lock() const noexcept -> bool { return mutex_ != nullptr; }
private:
    M* mutex_{};
};

class [[nodiscard]] Pair final {
public:
    Pair(Spin& a, Spin& b) noexcept : first_(&a), second_(&b) {
        if (reinterpret_cast<usize>(first_) > reinterpret_cast<usize>(second_))
            std::swap(first_, second_);
        first_->lock();
        if (second_ != first_) second_->lock();
        else second_ = nullptr;
    }
    Pair(const Pair&) = delete;
    auto operator=(const Pair&) -> Pair& = delete;
    ~Pair() noexcept { release(); }
    void release() noexcept {
        if (!first_) return;
        if (second_) second_->unlock();
        first_->unlock();
        first_ = second_ = nullptr;
        irq_.restore();
    }
private:
    Irq irq_;
    Spin* first_{};
    Spin* second_{};
};

// One-shot countdown completion. arm() closes the complete-before-block race:
// true means the caller must block and exactly one notification will follow;
// false means completion was already visible and no notification was emitted.
class Latch final : private libk::noncopyable_nonmovable {
public:
    using Notifier = libk::delegate<void() noexcept>;

    explicit Latch(Notifier notifier = {}) noexcept
        : notifier_(notifier) {}
    ~Latch() noexcept {
        libk_assert(!initialized() || complete());
    }

    [[nodiscard]] auto initialized() const noexcept -> bool {
        return state_.load<libk::MemoryOrder::Acquire>() != State::Empty;
    }
    [[nodiscard]] auto complete() const noexcept -> bool {
        return state_.load<libk::MemoryOrder::Acquire>() == State::Complete;
    }
    [[nodiscard]] auto notifiable() const noexcept -> bool {
        return static_cast<bool>(notifier_);
    }
    [[nodiscard]] auto arm() noexcept -> bool {
        libk_assert(initialized() && notifier_);
        State expected = State::Awaiting;
        if (state_.compare_exchange_strong<
                libk::MemoryOrder::AcqRel,
                libk::MemoryOrder::Acquire>(expected, State::Armed)) {
            return true;
        }
        libk_assert(expected == State::Complete);
        return false;
    }

    void initialize(usize pending) noexcept {
        libk_assert(!initialized());
        pending_.store<libk::MemoryOrder::Relaxed>(pending);
        state_.store<libk::MemoryOrder::Release>(
            pending == 0 ? State::Complete : State::Awaiting);
    }

    [[nodiscard]] auto acknowledge() noexcept -> bool {
        return acknowledge([]() noexcept {});
    }

    template<typename BeforeComplete>
    [[nodiscard]] auto acknowledge(BeforeComplete&& before_complete) noexcept
        -> bool {
        libk_assert(initialized());
        // Every acknowledgement publishes its before-complete work with
        // Release.  The final RMW must acquire that release sequence before
        // invoking the unique terminal owner; this is part of the generic
        // countdown contract, not a diagnostics-only ordering.
        const usize previous = pending_.fetch_sub<libk::MemoryOrder::AcqRel>(1);
        libk_assert(previous != 0);
        if (previous != 1) {
            return false;
        }

        before_complete();
        // Copy the non-owning target before publishing Complete. An awakened
        // owner may release this completion as soon as it observes that state.
        const Notifier notifier = notifier_;
        const State previous_state = state_.exchange<libk::MemoryOrder::AcqRel>(
            State::Complete);
        libk_assert(previous_state == State::Awaiting
            || previous_state == State::Armed);
        if (previous_state == State::Armed) {
            libk_assert(notifier);
            notifier();
        }
        return true;
    }

private:
    enum class State : u8 {
        Empty,
        Awaiting,
        Armed,
        Complete,
    };

    Notifier notifier_{};
    libk::Atomic<usize> pending_{};
    libk::Atomic<State> state_{State::Empty};
};

} // namespace sync
