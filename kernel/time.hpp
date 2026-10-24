#pragma once
#include <libk/intrusive_tree.hpp>
#include <libk/noncopyable.hpp>
#include <libk/delegate.hpp>

#include <libk/types.hpp>
#include <cpu.hpp>
#include <libk/assert.hpp>
#include <libk/checked_arithmetic.hpp>
#include <concepts>
#include <ratio>
#include <optional>

namespace time {

using ns = std::nano;
using us = std::micro;
using ms = std::milli;
using s = std::ratio<1>;

// One system counter domain, shared by all CPUs. The selected architecture
// guarantees a common epoch/rate and programs timers in this same domain.
// Device counters are not system ticks, even when their frequencies match.
class Clock final {
public:
    explicit constexpr Clock(u64 hz) noexcept : hz_(hz) { libk_assert(hz != 0); }

    [[nodiscard]] constexpr auto rd_freq() const noexcept -> u64 {
        return hz_;
    }

    [[nodiscard]] auto rd_ticks() const noexcept -> u64 {
        return arch::read_clock();
    }

    // Physical units are seconds, milliseconds, microseconds or nanoseconds.
    // Round up so an interval is never shortened; avoid 128-bit division.
    template<class Unit, std::integral Rep>
        requires (sizeof(Rep) <= sizeof(u64)
            && (std::same_as<Unit, s> || std::same_as<Unit, ms>
                || std::same_as<Unit, us> || std::same_as<Unit, ns>))
    [[nodiscard]] constexpr auto ticks(Rep n) const noexcept
        -> std::optional<u64> {
        if constexpr (std::signed_integral<Rep>) {
            if (n < 0) return std::nullopt;
        }
        constexpr u64 scale = Unit::den;
        const u64 count = static_cast<u64>(n);
        const auto whole = libk::checked_multiply(count / scale, hz_);
        if (!whole) return std::nullopt;
        const u64 rem = count % scale;
        // rem * (frequency % scale) < 1e18; the rounded fractional
        // contribution is at most one second of ticks.
        const u64 product = rem * (hz_ % scale);
        const u64 part = rem * (hz_ / scale)
            + product / scale + (product % scale != 0);
        return libk::checked_add(*whole, part);
    }

    template<u64 n, class Unit>
    [[nodiscard]] constexpr auto ticks() const noexcept -> std::optional<u64> {
        return ticks<Unit>(n);
    }

private:
    const u64 hz_;
};

} // namespace time

namespace sched {
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
