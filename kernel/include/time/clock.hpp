#pragma once

#include <core/types.hpp>
#include <libk/noncopyable.hpp>
#include <libk/checked_arithmetic.hpp>
#include <libk/optional.hpp>
#include <time/time.hpp>

namespace kernel::time {

// Kernel-lifetime validated clock configuration. Instant and Duration are
// expressed in this clock's ticks; conversion from policy-facing nanoseconds
// is centralized here so scheduler code never performs frequency arithmetic.
class Clock final : private libk::noncopyable_nonmovable {
public:
    explicit constexpr Clock(u64 ticks_per_second) noexcept
        : ticks_per_second_(ticks_per_second) {}

    [[nodiscard]] constexpr auto valid() const noexcept -> bool {
        return ticks_per_second_ != 0;
    }

    [[nodiscard]] constexpr auto ticks_per_second() const noexcept -> u64 {
        return ticks_per_second_;
    }

    [[nodiscard]] auto now() const noexcept -> Instant;

    // Rounds up so a configured interval is never shortened by conversion.
    [[nodiscard]] constexpr auto duration_from_nanoseconds(
        u64 nanoseconds) const noexcept -> libk::optional<Duration> {
        if (!valid()) {
            return libk::nullopt;
        }

        constexpr u64 nanoseconds_per_second = 1'000'000'000ULL;
        const u64 whole_seconds = nanoseconds / nanoseconds_per_second;
        const u64 remainder = nanoseconds % nanoseconds_per_second;

        const auto whole_ticks =
            libk::checked_multiply(whole_seconds, ticks_per_second_);
        if (!whole_ticks) {
            return libk::nullopt;
        }

        // Compute ceil(remainder * frequency / 1e9) without requiring a
        // compiler-provided 128-bit division runtime.
        const u64 frequency_quotient =
            ticks_per_second_ / nanoseconds_per_second;
        const u64 frequency_remainder =
            ticks_per_second_ % nanoseconds_per_second;
        const auto quotient_ticks =
            libk::checked_multiply(remainder, frequency_quotient);
        const auto remainder_product =
            libk::checked_multiply(remainder, frequency_remainder);
        if (!quotient_ticks || !remainder_product) {
            return libk::nullopt;
        }

        const u64 divided = *remainder_product / nanoseconds_per_second;
        const u64 round_up =
            (*remainder_product % nanoseconds_per_second) != 0 ? 1 : 0;
        const auto with_divided =
            libk::checked_add(*quotient_ticks, divided);
        if (!with_divided) {
            return libk::nullopt;
        }
        const auto rounded = libk::checked_add(*with_divided, round_up);
        if (!rounded) {
            return libk::nullopt;
        }
        const auto total = libk::checked_add(*whole_ticks, *rounded);
        if (!total) {
            return libk::nullopt;
        }
        return Duration::from_ticks(*total);
    }

private:
    u64 ticks_per_second_{};
};

} // namespace kernel::time
