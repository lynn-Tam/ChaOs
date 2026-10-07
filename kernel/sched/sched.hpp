#pragma once

#include <base/types.hpp>
#include <optional>
#include <libk/assert.hpp>
#include <libk/noncopyable.hpp>
#include <libk/delegate.hpp>
#include <libk/intrusive_tree.hpp>
#include <time/time.hpp>

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
    using Callback = libk::delegate<void() noexcept>;

    explicit Deadline(Callback callback) noexcept : callback_(callback) {}
    ~Deadline() noexcept { libk_assert(!armed() && !hook_.is_linked() && callback_); }

    [[nodiscard]] auto armed() const noexcept -> bool {
        return owner_ != nullptr;
    }

private:
    friend class Dispatcher;

    libk::IntrusiveTreeHook hook_{};
    Callback callback_{};
    Dispatcher* owner_{};
    time::Instant when_{};
};

} // namespace sched
