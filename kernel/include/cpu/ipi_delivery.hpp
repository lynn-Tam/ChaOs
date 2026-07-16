#pragma once

#include <core/types.hpp>
#include <libk/optional.hpp>

namespace kernel {

// Edge-delivery state for canonical per-CPU work queues. The queue remains the
// work truth; this protocol only proves that non-empty work either needs a
// transport kick, has one in flight, or retains a retry obligation.
//
// The enclosing queue lock serializes every operation. A generation token
// prevents a late transport error from demoting work that was already consumed
// or signaled by a newer kick.
class IpiDelivery final {
public:
    struct Token final {
        u64 generation{};

        [[nodiscard]] constexpr explicit operator bool() const noexcept {
            return generation != 0;
        }
    };

    void publish() noexcept {
        if (state_ == State::Idle) {
            state_ = State::NeedsKick;
        }
    }

    [[nodiscard]] auto claim() noexcept -> libk::optional<Token> {
        if (state_ != State::NeedsKick && state_ != State::Retry) {
            return libk::nullopt;
        }
        ++generation_;
        if (generation_ == 0) {
            // A transport generation is diagnostic identity rather than a
            // resource epoch. Skipping zero keeps default Token invalid.
            ++generation_;
        }
        state_ = State::InFlight;
        return Token{generation_};
    }

    void fail(Token token) noexcept {
        if (state_ == State::InFlight
            && token.generation == generation_) {
            state_ = State::Retry;
        }
    }

    void consume() noexcept {
        state_ = State::Idle;
    }

    [[nodiscard]] auto pending() const noexcept -> bool {
        return state_ != State::Idle;
    }

    [[nodiscard]] auto retry_needed() const noexcept -> bool {
        return state_ == State::Retry;
    }

private:
    enum class State : u8 {
        Idle,
        NeedsKick,
        InFlight,
        Retry,
    };

    State state_{State::Idle};
    u64 generation_{};
};

} // namespace kernel
