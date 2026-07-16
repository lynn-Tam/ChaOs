#pragma once

#include <stdint.h>

#include <libk/concepts.hpp>
#include <libk/assert.hpp>

namespace libk {

// Bit operations intentionally accept only unsigned, non-bool integral types.
// This keeps shifts and bit-pattern arithmetic free from signed-overflow rules.
inline constexpr unsigned bit_npos = ~0u;

template<UnsignedIntegral T>
[[nodiscard]] constexpr auto bit_digits() noexcept -> unsigned {
    return static_cast<unsigned>(sizeof(T) * __CHAR_BIT__);
}

template<UnsignedIntegral T>
[[nodiscard]] constexpr auto bit(unsigned index) noexcept -> T {
    libk_assert(index < bit_digits<T>());
    return static_cast<T>(T{1} << index);
}

template<UnsignedIntegral T>
[[nodiscard]] constexpr auto has_any(T value, T mask) noexcept -> bool {
    return (value & mask) != T{0};
}

template<UnsignedIntegral T>
[[nodiscard]] constexpr auto has_all(T value, T mask) noexcept -> bool {
    return (value & mask) == mask;
}

template<UnsignedIntegral T>
[[nodiscard]] constexpr auto set_bits(T value, T mask) noexcept -> T {
    return static_cast<T>(value | mask);
}

template<UnsignedIntegral T>
[[nodiscard]] constexpr auto clear_bits(T value, T mask) noexcept -> T {
    return static_cast<T>(value & static_cast<T>(~mask));
}

template<UnsignedIntegral T>
[[nodiscard]] constexpr auto toggle_bits(T value, T mask) noexcept -> T {
    return static_cast<T>(value ^ mask);
}

template<UnsignedIntegral T>
[[nodiscard]] constexpr auto low_mask(unsigned width) noexcept -> T {
    libk_assert(width <= bit_digits<T>());
    if (width == bit_digits<T>()) {
        return static_cast<T>(~T{0});
    }
    return static_cast<T>((T{1} << width) - T{1});
}

template<UnsignedIntegral T>
[[nodiscard]] constexpr auto field_mask(
    unsigned shift,
    unsigned width) noexcept -> T {
    libk_assert(shift <= bit_digits<T>());
    libk_assert(width <= bit_digits<T>() - shift);
    return width == 0
        ? T{0}
        : static_cast<T>(low_mask<T>(width) << shift);
}

template<UnsignedIntegral T>
[[nodiscard]] constexpr auto encode_field(
    T value,
    unsigned shift,
    unsigned width) noexcept -> T {
    libk_assert(shift <= bit_digits<T>());
    libk_assert(width <= bit_digits<T>() - shift);
    return width == 0
        ? T{0}
        : static_cast<T>((value & low_mask<T>(width)) << shift);
}

template<UnsignedIntegral T>
[[nodiscard]] constexpr auto replace_field(
    T original,
    T field_value,
    unsigned shift,
    unsigned width) noexcept -> T {
    const T mask = field_mask<T>(shift, width);
    return static_cast<T>(
        (original & static_cast<T>(~mask))
        | encode_field<T>(field_value, shift, width));
}

template<UnsignedIntegral T>
[[nodiscard]] constexpr auto extract_field(
    T value,
    unsigned shift,
    unsigned width) noexcept -> T {
    libk_assert(shift <= bit_digits<T>());
    libk_assert(width <= bit_digits<T>() - shift);
    return width == 0
        ? T{0}
        : static_cast<T>((value >> shift) & low_mask<T>(width));
}

namespace bits_detail {

enum class ZeroSide : uint8_t {
    Leading,
    Trailing,
};

template<ZeroSide side, unsigned width, UnsignedIntegral T>
[[nodiscard]] constexpr auto count_zero_step(
    T value,
    unsigned count) noexcept -> unsigned {
    if constexpr (width == 0) {
        return count;
    } else {
        bool half_is_zero = false;
        if constexpr (side == ZeroSide::Leading) {
            half_is_zero =
                (value >> (bit_digits<T>() - width)) == T{0};
        } else {
            half_is_zero = (value & low_mask<T>(width)) == T{0};
        }

        if (half_is_zero) {
            count += width;
            if constexpr (side == ZeroSide::Leading) {
                value = static_cast<T>(value << width);
            } else {
                value = static_cast<T>(value >> width);
            }
        }
        return count_zero_step<side, width / 2>(value, count);
    }
}

template<ZeroSide side, UnsignedIntegral T>
[[nodiscard]] constexpr auto count_zero(T value) noexcept -> unsigned {
    constexpr unsigned digits = bit_digits<T>();
    static_assert((digits & (digits - 1)) == 0);
    return value == T{0}
        ? digits
        : count_zero_step<side, digits / 2>(value, 0);
}

} // namespace bits_detail

template<UnsignedIntegral T>
[[nodiscard]] constexpr auto countl_zero(T value) noexcept -> unsigned {
    return bits_detail::count_zero<bits_detail::ZeroSide::Leading>(value);
}

template<UnsignedIntegral T>
[[nodiscard]] constexpr auto countr_zero(T value) noexcept -> unsigned {
    return bits_detail::count_zero<bits_detail::ZeroSide::Trailing>(value);
}

template<UnsignedIntegral T>
[[nodiscard]] constexpr auto popcount(T value) noexcept -> unsigned {
    static_assert(sizeof(T) <= sizeof(uint64_t));
    uint64_t bits = static_cast<uint64_t>(value);
    bits -= (bits >> 1) & UINT64_C(0x5555555555555555);
    bits = (bits & UINT64_C(0x3333333333333333))
        + ((bits >> 2) & UINT64_C(0x3333333333333333));
    bits = (bits + (bits >> 4)) & UINT64_C(0x0f0f0f0f0f0f0f0f);
    return static_cast<unsigned>(
        (bits * UINT64_C(0x0101010101010101)) >> 56);
}

template<UnsignedIntegral T>
[[nodiscard]] constexpr auto bit_width(T value) noexcept -> unsigned {
    return value == T{0}
        ? 0u
        : bit_digits<T>() - countl_zero(value);
}

template<UnsignedIntegral T>
[[nodiscard]] constexpr auto has_single_bit(T value) noexcept -> bool {
    return value != T{0}
        && (value & static_cast<T>(value - T{1})) == T{0};
}

template<UnsignedIntegral T>
[[nodiscard]] constexpr auto find_first_set(T value) noexcept -> unsigned {
    return value == T{0} ? bit_npos : countr_zero(value);
}

template<UnsignedIntegral T>
[[nodiscard]] constexpr auto find_last_set(T value) noexcept -> unsigned {
    return value == T{0}
        ? bit_npos
        : bit_digits<T>() - 1u - countl_zero(value);
}

} // namespace libk
