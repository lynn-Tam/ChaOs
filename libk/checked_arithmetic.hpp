#pragma once

#include <stddef.h>

#include <libk/bits.hpp>
#include <libk/limits.hpp>
#include <libk/optional.hpp>
#include <libk/typetraits.hpp>

namespace libk {

// Fallible arithmetic for freestanding unsigned integer paths where overflow
// is a recoverable input condition rather than an assertion failure.
template<typename T>
concept CheckedUnsignedInteger =
    is_integral_v<T>
    && requires {
        numeric_limits<T>::min();
        numeric_limits<T>::max();
    }
    && numeric_limits<T>::min() == 0;

template<CheckedUnsignedInteger T>
[[nodiscard]] constexpr auto checked_add(T lhs, T rhs) noexcept
    -> optional<T> {
    if (rhs > numeric_limits<T>::max() - lhs) {
        return nullopt;
    }
    return lhs + rhs;
}

template<CheckedUnsignedInteger T>
[[nodiscard]] constexpr auto checked_multiply(T lhs, T rhs) noexcept
    -> optional<T> {
    if (lhs != 0 && rhs > numeric_limits<T>::max() / lhs) {
        return nullopt;
    }
    return lhs * rhs;
}

template<CheckedUnsignedInteger T>
[[nodiscard]] constexpr auto checked_align_up(
    T value,
    size_t alignment) noexcept -> optional<T> {
    if (!has_single_bit(alignment)) {
        return nullopt;
    }
    if constexpr(sizeof(T) < sizeof(size_t)) {
        if (alignment > static_cast<size_t>(numeric_limits<T>::max())) {
            return nullopt;
        }
    }

    const T mask = static_cast<T>(alignment - 1);
    const auto adjusted = checked_add(value, mask);
    if (!adjusted.has_value()) {
        return nullopt;
    }

    return static_cast<T>(adjusted.value() & static_cast<T>(~mask));
}

} // namespace libk
