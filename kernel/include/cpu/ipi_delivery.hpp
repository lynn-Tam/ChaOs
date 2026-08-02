#pragma once

#include <core/types.hpp>
#include <libk/limits.hpp>
#include <libk/optional.hpp>
#include <libk/sync/atomic.hpp>

namespace kernel {

// Edge-delivery state for canonical per-CPU work queues. The queue remains the
// work truth; this protocol only proves that non-empty work either needs a
// transport kick, has one in flight, or retains a retry obligation.
//
// Queue-owned transitions run under the enclosing queue lock. Transport
// failure is the one lockless transition: its token must compare the exact
// state-plus-generation word so a late sender cannot demote a newer kick.
class IpiDelivery final {
public:
    enum class State : u8 {
        Idle,
        NeedsKick,
        InFlight,
        Retry,
    };

    struct Token final {
        u64 generation{};

        [[nodiscard]] constexpr explicit operator bool() const noexcept {
            return generation != 0;
        }
    };

    void publish() noexcept {
        const u64 observed = word_.load<libk::MemoryOrder::Acquire>();
        if (state(observed) != State::Idle) {
            return;
        }
        u64 expected = observed;
        static_cast<void>(word_.compare_exchange_strong<
            libk::MemoryOrder::AcqRel,
            libk::MemoryOrder::Acquire>(
            expected,
            pack(generation(observed), State::NeedsKick)));
    }

    [[nodiscard]] auto claim() noexcept -> libk::optional<Token> {
        const u64 observed = word_.load<libk::MemoryOrder::Acquire>();
        const State current = state(observed);
        if (current != State::NeedsKick && current != State::Retry) {
            return libk::nullopt;
        }
        const u64 previous_generation = generation(observed);
        u64 next_generation = previous_generation;
        if (next_generation == max_generation) {
            // A transport generation is diagnostic identity rather than a
            // resource epoch. Skipping zero keeps default Token invalid.
            next_generation = 1;
        } else {
            ++next_generation;
        }
        u64 expected = observed;
        if (!word_.compare_exchange_strong<
                libk::MemoryOrder::AcqRel,
                libk::MemoryOrder::Acquire>(
                expected,
                pack(next_generation, State::InFlight))) {
            // Queue-owned callers normally make this impossible. Returning
            // no token keeps a future caller responsible for the unchanged
            // queue edge if the lockless failure transition raced anyway.
            return libk::nullopt;
        }
        return Token{next_generation};
    }

    void fail(Token token) noexcept {
        static_cast<void>(fail_if_inflight(token));
    }

    // Transport failure runs after the queue lock has been released by the
    // best-effort producer. The packed expected word makes this CAS harmless
    // if the consumer has already consumed the queue or a newer transport has
    // taken ownership.
    [[nodiscard]] auto fail_if_inflight(Token token) noexcept -> bool {
        if (token.generation == 0 || token.generation > max_generation) {
            return false;
        }
        u64 expected = pack(token.generation, State::InFlight);
        return word_.compare_exchange_strong<
            libk::MemoryOrder::AcqRel,
            libk::MemoryOrder::Acquire>(
            expected,
            pack(token.generation, State::Retry));
    }

    void consume() noexcept {
        const u64 observed = word_.load<libk::MemoryOrder::Acquire>();
        word_.store<libk::MemoryOrder::Release>(
            pack(generation(observed), State::Idle));
    }

    [[nodiscard]] auto pending() const noexcept -> bool {
        return state(word_.load<libk::MemoryOrder::Acquire>())
            != State::Idle;
    }

    [[nodiscard]] auto retry_needed() const noexcept -> bool {
        return state(word_.load<libk::MemoryOrder::Acquire>())
            == State::Retry;
    }

    [[nodiscard]] auto state() const noexcept -> State {
        return state(word_.load<libk::MemoryOrder::Acquire>());
    }
    [[nodiscard]] auto generation() const noexcept -> u64 {
        return generation(word_.load<libk::MemoryOrder::Acquire>());
    }

private:
    static constexpr u32 state_bits = 2;
    static constexpr u64 state_mask = (u64{1} << state_bits) - 1;
    static constexpr u64 max_generation =
        libk::numeric_limits<u64>::max() >> state_bits;

    [[nodiscard]] static constexpr auto pack(
        u64 generation_value,
        State state_value) noexcept -> u64 {
        return (generation_value << state_bits)
            | static_cast<u64>(state_value);
    }

    [[nodiscard]] static constexpr auto state(u64 word) noexcept -> State {
        return static_cast<State>(word & state_mask);
    }

    [[nodiscard]] static constexpr auto generation(u64 word) noexcept -> u64 {
        return word >> state_bits;
    }

    libk::Atomic<u64> word_{};
};

} // namespace kernel
