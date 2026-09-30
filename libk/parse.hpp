#pragma once

#include <concepts>
#include <limits>
#include <optional>
#include <string_view>

namespace libk {

// Whole-string unsigned parsing. The current freestanding STL lacks charconv.
template<std::unsigned_integral T>
    requires (!std::same_as<T, bool>)
constexpr auto parse(std::string_view text, unsigned base = 10) noexcept -> std::optional<T> {
    if (text.empty() || base < 2 || base > 36) return std::nullopt;
    T value{};
    for (char c : text) {
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        const unsigned digit = c >= '0' && c <= '9' ? c - '0'
            : c >= 'a' && c <= 'z' ? c - 'a' + 10 : 36;
        if (digit >= base || value > (std::numeric_limits<T>::max() - digit) / base)
            return std::nullopt;
        value = value * base + digit;
    }
    return value;
}

} // namespace libk
