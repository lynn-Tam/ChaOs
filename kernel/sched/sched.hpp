#pragma once

#include <libk/types.hpp>
#include <optional>
#include <libk/assert.hpp>
#include <libk/noncopyable.hpp>
#include <libk/delegate.hpp>
#include <libk/intrusive_tree.hpp>

namespace sched {

class Urgency final {
public:
    static constexpr u8 level_count = 32;

    [[nodiscard]] static constexpr auto make(usize value) noexcept
        -> std::optional<Urgency> {
        if (value >= level_count) {
            return std::nullopt;
        }
        return Urgency{static_cast<u8>(value)};
    }

    [[nodiscard]] constexpr auto value() const noexcept -> u8 {
        return value_;
    }

    friend constexpr auto operator==(Urgency, Urgency) noexcept
        -> bool = default;

private:
    explicit constexpr Urgency(u8 value) noexcept : value_(value) {}

    u8 value_{};
};

enum class DispatchReason : u8 {
    Start,
    Yield,
    Timer,
    Block,
    Exit,
    Stop,
    RemoteWake,
};

class Dispatcher;

// Fixed-storage one-shot deadline owned by exactly one Dispatcher while
// armed. The callback runs on that CPU with interrupts disabled; remote
// producers publish subsystem state and wake the owner instead of mutating
// this queue.
class Deadline final : private libk::noncopyable_nonmovable {
public:
    using Fn = libk::delegate<void() noexcept>;

    explicit Deadline(Fn cb) noexcept : cb_(cb) {}
    ~Deadline() noexcept { libk_assert(!armed() && !hook_.is_linked() && cb_); }

    [[nodiscard]] auto armed() const noexcept -> bool {
        return owner_ != nullptr;
    }

private:
    friend class Dispatcher;

    libk::intrusive_tree_hook hook_{};
    Fn cb_{};
    Dispatcher* owner_{};
    u64 when_{};
};

} // namespace sched
