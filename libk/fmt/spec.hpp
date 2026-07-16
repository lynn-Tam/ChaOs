#pragma once

#include <stddef.h>
#include <stdint.h>

#include "libk/string_view.hpp"
#include "libk/fmt/error.hpp"

namespace libk::fmt {

enum class align : uint8_t {
    none,
    left,
    right,
    center,
};

enum class sign : uint8_t {
    minus_only,
    plus,
    space,
};

struct format_spec {
    char fill{' '};
    align alignment{align::none};
    sign sign_mode{sign::minus_only};
    bool alternate{};
    bool zero_pad{};
    bool has_width{};
    size_t width{};
    bool has_precision{};
    size_t precision{};
    char type{};
};

struct replacement_field {
    bool has_index{};
    size_t index{};
    format_spec spec{};
};

namespace detail {

constexpr bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

constexpr bool is_align_char(char c) noexcept {
    return c == '<' || c == '>' || c == '^';
}

constexpr align parse_align(char c) noexcept {
    if (c == '<') return align::left;
    if (c == '>') return align::right;
    if (c == '^') return align::center;
    return align::none;
}

constexpr bool checked_add_digit(size_t& value, char c) noexcept {
    constexpr size_t max = static_cast<size_t>(~static_cast<size_t>(0));
    const size_t digit = static_cast<size_t>(c - '0');
    if (value > (max - digit) / 10) {
        return false;
    }
    value = value * 10 + digit;
    return true;
}

constexpr bool parse_unsigned(StrView text, size_t& pos, size_t& out) noexcept {
    if (pos >= text.size() || !is_digit(text[pos])) {
        return false;
    }
    size_t value = 0;
    while (pos < text.size() && is_digit(text[pos])) {
        if (!checked_add_digit(value, text[pos])) {
            return false;
        }
        ++pos;
    }
    out = value;
    return true;
}

constexpr errc parse_format_spec(StrView text, size_t& pos, format_spec& spec) noexcept {
    if (pos >= text.size() || text[pos] == '}') {
        return errc::ok;
    }

    if ((pos + 1) < text.size() && is_align_char(text[pos + 1])) {
        spec.fill = text[pos];
        spec.alignment = parse_align(text[pos + 1]);
        pos += 2;
    } else if (is_align_char(text[pos])) {
        spec.alignment = parse_align(text[pos]);
        ++pos;
    }

    if (pos < text.size()) {
        if (text[pos] == '+') {
            spec.sign_mode = sign::plus;
            ++pos;
        } else if (text[pos] == '-') {
            spec.sign_mode = sign::minus_only;
            ++pos;
        } else if (text[pos] == ' ') {
            spec.sign_mode = sign::space;
            ++pos;
        }
    }

    if (pos < text.size() && text[pos] == '#') {
        spec.alternate = true;
        ++pos;
    }

    if (pos < text.size() && text[pos] == '0') {
        spec.zero_pad = true;
        if (spec.alignment == align::none) {
            spec.alignment = align::right;
            spec.fill = '0';
        }
        ++pos;
    }

    if (pos < text.size() && is_digit(text[pos])) {
        spec.has_width = true;
        if (!parse_unsigned(text, pos, spec.width)) {
            return errc::bad_spec;
        }
    }

    if (pos < text.size() && text[pos] == '.') {
        ++pos;
        spec.has_precision = true;
        if (!parse_unsigned(text, pos, spec.precision)) {
            return errc::bad_spec;
        }
    }

    if (pos < text.size() && text[pos] != '}') {
        spec.type = text[pos];
        ++pos;
    }

    if (pos >= text.size() || text[pos] != '}') {
        return errc::bad_spec;
    }
    return errc::ok;
}

constexpr errc parse_replacement_field(StrView text, size_t& pos, replacement_field& field) noexcept {
    field = replacement_field{};

    if (pos < text.size() && is_digit(text[pos])) {
        field.has_index = true;
        if (!parse_unsigned(text, pos, field.index)) {
            return errc::bad_format;
        }
    }

    if (pos < text.size() && text[pos] == ':') {
        ++pos;
        const errc spec_error = parse_format_spec(text, pos, field.spec);
        if (spec_error != errc::ok) {
            return spec_error;
        }
    } else if (pos >= text.size() || text[pos] != '}') {
        return errc::bad_format;
    }

    ++pos;
    return errc::ok;
}

} // namespace detail

} // namespace libk::fmt
