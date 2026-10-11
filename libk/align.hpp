#pragma once

#include <stddef.h>
#include <stdint.h>

#include <libk/assert.hpp>
#include <libk/checked_arithmetic.hpp>
#include <libk/concepts.hpp>

namespace libk {

template<unsigned_integer T>
[[nodiscard]] constexpr auto align_up(T value, size_t alignment) noexcept -> T {
    const auto aligned = checked_align_up(value, alignment);
    libk_assert(aligned.has_value());
    return aligned.value();
}

template<class T>
    requires std::is_pointer_v<T>
[[nodiscard]] auto align_up(T value, size_t alignment) noexcept -> T {
    const auto aligned = checked_align_up(
        reinterpret_cast<uintptr_t>(value), alignment);
    libk_assert(aligned.has_value());
    return reinterpret_cast<T>(aligned.value());
}

} // namespace libk
