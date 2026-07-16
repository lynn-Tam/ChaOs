#pragma once

#include <core/types.hpp>
#include <libk/checked_arithmetic.hpp>
#include <libk/limits.hpp>
#include <libk/optional.hpp>

namespace kernel::time {

class Duration final {
public:
    constexpr Duration() noexcept = default;

    [[nodiscard]] static constexpr auto from_ticks(u64 ticks) noexcept
        -> Duration {
        return Duration{ticks};
    }

    [[nodiscard]] constexpr auto ticks() const noexcept -> u64 {
        return ticks_;
    }

    [[nodiscard]] constexpr auto empty() const noexcept -> bool {
        return ticks_ == 0;
    }

    friend constexpr auto operator==(Duration lhs, Duration rhs) noexcept
        -> bool {
        return lhs.ticks_ == rhs.ticks_;
    }
    friend constexpr auto operator<(Duration lhs, Duration rhs) noexcept
        -> bool {
        return lhs.ticks_ < rhs.ticks_;
    }
    friend constexpr auto operator<=(Duration lhs, Duration rhs) noexcept
        -> bool {
        return lhs.ticks_ <= rhs.ticks_;
    }
    friend constexpr auto operator>(Duration lhs, Duration rhs) noexcept
        -> bool {
        return rhs < lhs;
    }
    friend constexpr auto operator>=(Duration lhs, Duration rhs) noexcept
        -> bool {
        return rhs <= lhs;
    }

private:
    explicit constexpr Duration(u64 ticks) noexcept : ticks_(ticks) {}

    u64 ticks_{};
};

class Instant final {
public:
    constexpr Instant() noexcept = default;

    [[nodiscard]] static constexpr auto from_ticks(u64 ticks) noexcept
        -> Instant {
        return Instant{ticks};
    }

    [[nodiscard]] static constexpr auto max() noexcept -> Instant {
        return Instant{libk::numeric_limits<u64>::max()};
    }

    [[nodiscard]] constexpr auto ticks() const noexcept -> u64 {
        return ticks_;
    }

    [[nodiscard]] constexpr auto checked_add(Duration duration) const noexcept
        -> libk::optional<Instant> {
        const auto result = libk::checked_add(ticks_, duration.ticks());
        if (!result) {
            return libk::nullopt;
        }
        return Instant{*result};
    }

    [[nodiscard]] constexpr auto elapsed_since(Instant earlier) const noexcept
        -> libk::optional<Duration> {
        if (*this < earlier) {
            return libk::nullopt;
        }
        return Duration::from_ticks(ticks_ - earlier.ticks_);
    }

    friend constexpr auto operator==(Instant lhs, Instant rhs) noexcept
        -> bool {
        return lhs.ticks_ == rhs.ticks_;
    }
    friend constexpr auto operator<(Instant lhs, Instant rhs) noexcept
        -> bool {
        return lhs.ticks_ < rhs.ticks_;
    }
    friend constexpr auto operator<=(Instant lhs, Instant rhs) noexcept
        -> bool {
        return lhs.ticks_ <= rhs.ticks_;
    }
    friend constexpr auto operator>(Instant lhs, Instant rhs) noexcept
        -> bool {
        return rhs < lhs;
    }
    friend constexpr auto operator>=(Instant lhs, Instant rhs) noexcept
        -> bool {
        return rhs <= lhs;
    }

private:
    explicit constexpr Instant(u64 ticks) noexcept : ticks_(ticks) {}

    u64 ticks_{};
};

static_assert(sizeof(Duration) == sizeof(u64));
static_assert(sizeof(Instant) == sizeof(u64));

} // namespace kernel::time
