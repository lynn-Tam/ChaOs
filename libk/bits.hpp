#pragma once

#include <stdint.h>
#include <bit>
#include <concepts>
#include <optional>
#include <type_traits>
#include <utility>

#include <libk/concepts.hpp>
#include <libk/assert.hpp>

namespace libk {

// Enum values normally encode bits. An explicit projection also supports
// ordinal enums without changing their hardware or wire representation.
template<typename E, auto encode = std::to_underlying<E>>
    requires std::is_enum_v<E>
class enum_flags final {
    using word = std::underlying_type_t<E>;
    static_assert(std::unsigned_integral<word>);
public:
    constexpr enum_flags() noexcept = default;

    template<typename... T>
        requires (std::same_as<T, E> && ...)
    [[nodiscard]] static constexpr auto of(T... values) noexcept -> enum_flags {
        return from_raw(static_cast<word>((word{} | ... | encode(values))));
    }
    [[nodiscard]] constexpr auto contains(E value) const noexcept -> bool {
        return contains(of(value));
    }
    [[nodiscard]] constexpr auto contains(enum_flags other) const noexcept -> bool {
        return (bits_ & other.bits_) == other.bits_;
    }
    [[nodiscard]] constexpr auto intersect(enum_flags other) const noexcept -> enum_flags {
        return from_raw(static_cast<word>(bits_ & other.bits_));
    }
    [[nodiscard]] constexpr auto empty() const noexcept -> bool { return bits_ == 0; }
    [[nodiscard]] constexpr auto raw() const noexcept -> word { return bits_; }

    // Raw construction preserves bits; callers apply their domain's policy.
    [[nodiscard]] static constexpr auto from_raw(word bits) noexcept -> enum_flags {
        enum_flags value;
        value.bits_ = bits;
        return value;
    }
    [[nodiscard]] static constexpr auto parse(word bits, word allowed) noexcept
        -> std::optional<enum_flags> {
        return (bits & ~allowed) == 0
            ? std::optional<enum_flags>{from_raw(bits)} : std::nullopt;
    }
    friend constexpr auto operator==(enum_flags, enum_flags) noexcept -> bool = default;
private:
    word bits_{};
};

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

template<UnsignedIntegral T>
[[nodiscard]] constexpr auto find_first_set(T value) noexcept -> unsigned {
    return value == T{0} ? bit_npos : static_cast<unsigned>(std::countr_zero(value));
}

template<UnsignedIntegral T>
[[nodiscard]] constexpr auto find_last_set(T value) noexcept -> unsigned {
    return value == T{0}
        ? bit_npos
        : bit_digits<T>() - 1u - static_cast<unsigned>(std::countl_zero(value));
}

} // namespace libk
