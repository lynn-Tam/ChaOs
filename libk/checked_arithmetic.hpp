#pragma once

#include <stddef.h>

#include <bit>
#include <limits>
#include <optional>
#include <type_traits>

namespace libk {

// Fallible arithmetic for freestanding unsigned integer paths where overflow
// is a recoverable input condition rather than an assertion failure.
template<typename T>
concept CheckedUnsignedInteger =
    std::is_integral_v<T>
    && requires {
        std::numeric_limits<T>::min();
        std::numeric_limits<T>::max();
    }
    && std::numeric_limits<T>::min() == 0;

template<CheckedUnsignedInteger T>
[[nodiscard]] constexpr auto checked_add(T lhs, T rhs) noexcept
    -> std::optional<T> {
    if (rhs > std::numeric_limits<T>::max() - lhs) {
        return std::nullopt;
    }
    return lhs + rhs;
}

template<CheckedUnsignedInteger T>
[[nodiscard]] constexpr auto checked_multiply(T lhs, T rhs) noexcept
    -> std::optional<T> {
    if (lhs != 0 && rhs > std::numeric_limits<T>::max() / lhs) {
        return std::nullopt;
    }
    return lhs * rhs;
}

template<CheckedUnsignedInteger T>
[[nodiscard]] constexpr auto checked_align_up(
    T value,
    size_t alignment) noexcept -> std::optional<T> {
    if (!std::has_single_bit(alignment)) {
        return std::nullopt;
    }
    if constexpr(sizeof(T) < sizeof(size_t)) {
        if (alignment > static_cast<size_t>(std::numeric_limits<T>::max())) {
            return std::nullopt;
        }
    }

    const T mask = static_cast<T>(alignment - 1);
    const auto adjusted = checked_add(value, mask);
    if (!adjusted.has_value()) {
        return std::nullopt;
    }

    return static_cast<T>(adjusted.value() & static_cast<T>(~mask));
}

} // namespace libk
