#pragma once

#include <core/debug.hpp>
#include <core/types.hpp>
#include <libk/delegate.hpp>
#include <libk/noncopyable.hpp>
#include <libk/sync/atomic.hpp>

namespace kernel::sync {

// One-shot countdown completion. arm() closes the complete-before-block race:
// true means the caller must block and exactly one notification will follow;
// false means completion was already visible and no notification was emitted.
class Completion final : private libk::noncopyable_nonmovable {
public:
    using Notifier = libk::delegate<void() noexcept>;

    explicit Completion(Notifier notifier = {}) noexcept
        : notifier_(notifier) {}
    ~Completion() noexcept {
        KASSERT(!initialized() || complete());
    }

    [[nodiscard]] auto initialized() const noexcept -> bool {
        return initialized_;
    }
    [[nodiscard]] auto complete() const noexcept -> bool {
        return initialized_
            && state_.load<libk::MemoryOrder::Acquire>() == State::Complete;
    }
    [[nodiscard]] auto notifiable() const noexcept -> bool {
        return static_cast<bool>(notifier_);
    }
    [[nodiscard]] auto arm() noexcept -> bool {
        KASSERT(initialized_ && notifier_);
        State expected = State::Awaiting;
        if (state_.compare_exchange_strong<
                libk::MemoryOrder::AcqRel,
                libk::MemoryOrder::Acquire>(expected, State::Armed)) {
            return true;
        }
        KASSERT(expected == State::Complete);
        return false;
    }

    void initialize(usize pending) noexcept {
        KASSERT(!initialized_);
        pending_.store<libk::MemoryOrder::Relaxed>(pending);
        initialized_ = true;
        if (pending == 0) {
            state_.store<libk::MemoryOrder::Release>(State::Complete);
        }
    }

    [[nodiscard]] auto acknowledge() noexcept -> bool {
        return acknowledge([]() noexcept {});
    }

    template<typename BeforeComplete>
    [[nodiscard]] auto acknowledge(BeforeComplete&& before_complete) noexcept
        -> bool {
        KASSERT(initialized_);
        // Every acknowledgement publishes its before-complete work with
        // Release.  The final RMW must acquire that release sequence before
        // invoking the unique terminal owner; this is part of the generic
        // countdown contract, not a diagnostics-only ordering.
        const usize previous = pending_.fetch_sub<libk::MemoryOrder::AcqRel>(1);
        KASSERT(previous != 0);
        if (previous != 1) {
            return false;
        }

        before_complete();
        // Copy the non-owning target before publishing Complete. An awakened
        // owner may release this completion as soon as it observes that state.
        const Notifier notifier = notifier_;
        const State previous_state = state_.exchange<libk::MemoryOrder::AcqRel>(
            State::Complete);
        KASSERT(previous_state == State::Awaiting
            || previous_state == State::Armed);
        if (previous_state == State::Armed) {
            KASSERT(notifier);
            notifier();
        }
        return true;
    }

private:
    enum class State : u8 {
        Awaiting,
        Armed,
        Complete,
    };

    Notifier notifier_{};
    libk::Atomic<usize> pending_{};
    libk::Atomic<State> state_{State::Awaiting};
    bool initialized_{};
};

} // namespace kernel::sync
