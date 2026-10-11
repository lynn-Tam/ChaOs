#pragma once

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
