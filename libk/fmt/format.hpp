#pragma once

#include <stddef.h>
#include <stdint.h>

#include "libk/concepts.hpp"
#include "libk/string_view.hpp"
#include "libk/fmt/error.hpp"
#include "libk/fmt/output.hpp"
#include "libk/fmt/spec.hpp"
#include "libk/typetraits.hpp"
#include "libk/utility.hpp"

namespace libk::fmt {

template<typename T>
struct formatter {};

namespace detail {

template<typename T>
struct is_signed_integer : false_type {};
template<> struct is_signed_integer<signed char> : true_type {};
template<> struct is_signed_integer<short> : true_type {};
template<> struct is_signed_integer<int> : true_type {};
template<> struct is_signed_integer<long> : true_type {};
template<> struct is_signed_integer<long long> : true_type {};

template<typename T>
struct is_unsigned_integer : false_type {};
template<> struct is_unsigned_integer<unsigned char> : true_type {};
template<> struct is_unsigned_integer<unsigned short> : true_type {};
template<> struct is_unsigned_integer<unsigned int> : true_type {};
template<> struct is_unsigned_integer<unsigned long> : true_type {};
template<> struct is_unsigned_integer<unsigned long long> : true_type {};

template<typename T>
struct is_char_array : false_type {};
template<size_t N> struct is_char_array<char[N]> : true_type {};
template<size_t N> struct is_char_array<const char[N]> : true_type {};

template<typename T>
struct is_c_string_pointer : false_type {};
template<> struct is_c_string_pointer<char*> : true_type {};
template<> struct is_c_string_pointer<const char*> : true_type {};

template<typename T>
inline constexpr bool is_signed_integer_v = is_signed_integer<remove_cvr_t<T>>::value;

template<typename T>
inline constexpr bool is_unsigned_integer_v = is_unsigned_integer<remove_cvr_t<T>>::value;

template<typename T, typename Ctx>
concept has_custom_formatter = requires(formatter<T> f, const T& value, Ctx& ctx, const format_spec& spec) {
    f.format(value, ctx, spec);
};

constexpr size_t cstr_len(const char* text) noexcept {
    if (text == nullptr) {
        return 0;
    }
    size_t n = 0;
    while (text[n] != '\0') {
        ++n;
    }
    return n;
}

constexpr bool is_printable_ascii(char c) noexcept {
    const unsigned char u = static_cast<unsigned char>(c);
    return u >= 0x20 && u <= 0x7e;
}

constexpr char lower_hex_digit(unsigned value) noexcept {
    return static_cast<char>((value < 10) ? ('0' + value) : ('a' + (value - 10)));
}

template<typename Ctx>
constexpr bool write_repeat(Ctx& ctx, char fill, size_t count) noexcept {
    for (size_t i = 0; i < count; ++i) {
        if (!ctx.put(fill)) {
            return false;
        }
    }
    return true;
}

template<typename Ctx, typename Writer>
constexpr bool write_aligned(Ctx& ctx, size_t content_len, const format_spec& spec,
    align default_alignment, Writer writer) noexcept {
    if (!spec.has_width || spec.width <= content_len) {
        return writer();
    }

    align use_align = spec.alignment;
    if (use_align == align::none) {
        use_align = default_alignment;
    }

    const size_t padding = spec.width - content_len;
    size_t left = 0;
    size_t right = 0;

    switch (use_align) {
    case align::left:
        right = padding;
        break;
    case align::center:
        left = padding / 2;
        right = padding - left;
        break;
    case align::none:
    case align::right:
        left = padding;
        break;
    }

    return write_repeat(ctx, spec.fill, left) && writer() && write_repeat(ctx, spec.fill, right);
}

template<typename Ctx>
constexpr bool write_str(Ctx& ctx, const char* text, size_t n) noexcept {
    return ctx.write(text, n);
}

constexpr bool spec_has_numeric_only_flags(const format_spec& spec) noexcept {
    return spec.sign_mode != sign::minus_only || spec.alternate || spec.zero_pad;
}

template<typename Ctx>
constexpr bool write_string(Ctx& ctx, StrView view, const format_spec& spec) noexcept {
    if (spec.type != 0 && spec.type != 's' && spec.type != '?') {
        ctx.fail(errc::bad_spec);
        return false;
    }
    if (spec_has_numeric_only_flags(spec)) {
        ctx.fail(errc::bad_spec);
        return false;
    }

    size_t n = view.size();
    if (spec.has_precision && spec.precision < n) {
        n = spec.precision;
    }

    if (spec.type == '?') {
        size_t escaped_len = 2;
        for (size_t i = 0; i < n; ++i) {
            const char c = view[i];
            switch (c) {
            case '\\':
            case '"':
            case '\n':
            case '\r':
            case '\t':
                escaped_len += 2;
                break;
            default:
                escaped_len += is_printable_ascii(c) ? size_t{1} : size_t{4};
                break;
            }
        }

        return write_aligned(ctx, escaped_len, spec, align::left, [&]() noexcept -> bool {
            if (!ctx.put('"')) return false;
            for (size_t i = 0; i < n; ++i) {
                const char c = view[i];
                switch (c) {
                case '\\': if (!ctx.write("\\\\", 2)) return false; break;
                case '"': if (!ctx.write("\\\"", 2)) return false; break;
                case '\n': if (!ctx.write("\\n", 2)) return false; break;
                case '\r': if (!ctx.write("\\r", 2)) return false; break;
                case '\t': if (!ctx.write("\\t", 2)) return false; break;
                default:
                    if (is_printable_ascii(c)) {
                        if (!ctx.put(c)) return false;
                    } else {
                        const unsigned char u = static_cast<unsigned char>(c);
                        char escaped[4] = {'\\', 'x', lower_hex_digit(u >> 4), lower_hex_digit(u & 0x0f)};
                        if (!ctx.write(escaped, 4)) return false;
                    }
                    break;
                }
            }
            return ctx.put('"');
        });
    }

    return write_aligned(ctx, n, spec, align::left, [&]() noexcept -> bool {
        return write_str(ctx, view.data(), n);
    });
}

template<typename Ctx>
constexpr bool write_char(Ctx& ctx, char value, const format_spec& spec) noexcept {
    if (spec.type != 0 && spec.type != 'c' && spec.type != '?') {
        ctx.fail(errc::bad_spec);
        return false;
    }
    if (spec_has_numeric_only_flags(spec)) {
        ctx.fail(errc::bad_spec);
        return false;
    }
    if (spec.has_precision) {
        ctx.fail(errc::bad_spec);
        return false;
    }

    if (spec.type == '?') {
        char escaped[6]{};
        size_t n = 0;
        escaped[n++] = '\'';
        switch (value) {
        case '\\': escaped[n++] = '\\'; escaped[n++] = '\\'; break;
        case '\'': escaped[n++] = '\\'; escaped[n++] = '\''; break;
        case '\n': escaped[n++] = '\\'; escaped[n++] = 'n'; break;
        case '\r': escaped[n++] = '\\'; escaped[n++] = 'r'; break;
        case '\t': escaped[n++] = '\\'; escaped[n++] = 't'; break;
        default:
            if (is_printable_ascii(value)) {
                escaped[n++] = value;
            } else {
                const unsigned char u = static_cast<unsigned char>(value);
                escaped[n++] = '\\';
                escaped[n++] = 'x';
                escaped[n++] = lower_hex_digit(u >> 4);
                escaped[n++] = lower_hex_digit(u & 0x0f);
            }
            break;
        }
        escaped[n++] = '\'';
        return write_aligned(ctx, n, spec, align::left, [&]() noexcept -> bool {
            return ctx.write(escaped, n);
        });
    }

    return write_aligned(ctx, 1, spec, align::left, [&]() noexcept -> bool {
        return ctx.put(value);
    });
}

struct unsigned_digits {
    char storage[65]{};
    size_t begin{};
    size_t size{};

    [[nodiscard]] constexpr auto data() const noexcept -> const char* {
        return storage + begin;
    }
};

constexpr unsigned_digits make_digits(uint64_t value, unsigned base, bool uppercase) noexcept {
    unsigned_digits out{};
    size_t pos = sizeof(out.storage);
    const char* digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        out.storage[--pos] = digits[value % base];
        value /= base;
    } while (value != 0);
    out.begin = pos;
    out.size = sizeof(out.storage) - pos;
    return out;
}

constexpr uint64_t signed_abs_to_u64(int64_t value) noexcept {
    const uint64_t raw = static_cast<uint64_t>(value);
    return value < 0 ? (~raw + 1u) : raw;
}

struct number_layout {
    char sign_char{};
    const char* prefix{};
    size_t prefix_len{};
    unsigned_digits digits{};
};

template<typename Ctx>
constexpr bool write_number_layout(Ctx& ctx, const number_layout& layout, const format_spec& spec) noexcept {
    const size_t sign_len = layout.sign_char ? 1u : 0u;
    const size_t content_len = sign_len + layout.prefix_len + layout.digits.size;

    const bool numeric_zero_padding = spec.has_width
        && spec.width > content_len
        && spec.alignment == align::right
        && spec.fill == '0';

    if (numeric_zero_padding) {
        const size_t zero_count = spec.width - content_len;
        if (layout.sign_char && !ctx.put(layout.sign_char)) return false;
        if (layout.prefix_len && !ctx.write(layout.prefix, layout.prefix_len)) return false;
        return write_repeat(ctx, '0', zero_count)
            && ctx.write(layout.digits.data(), layout.digits.size);
    }

    return write_aligned(ctx, content_len, spec, align::right, [&]() noexcept -> bool {
        if (layout.sign_char && !ctx.put(layout.sign_char)) return false;
        if (layout.prefix_len && !ctx.write(layout.prefix, layout.prefix_len)) return false;
        return ctx.write(layout.digits.data(), layout.digits.size);
    });
}

template<typename Ctx>
constexpr bool write_unsigned_number(Ctx& ctx, uint64_t value, const format_spec& spec,
    bool negative = false) noexcept {
    if (spec.has_precision) {
        ctx.fail(errc::bad_spec);
        return false;
    }

    char type = spec.type ? spec.type : 'd';
    if (type == 'u') {
        type = 'd';
    }

    if (type == 'c') {
        if (spec.alternate || spec.zero_pad || spec.sign_mode != sign::minus_only || value > 0xffu) {
            ctx.fail(errc::bad_spec);
            return false;
        }
        return write_char(ctx, static_cast<char>(value), format_spec{
            .fill = spec.fill,
            .alignment = spec.alignment,
            .has_width = spec.has_width,
            .width = spec.width,
        });
    }

    unsigned base = 10;
    bool uppercase = false;
    const char* prefix = "";
    size_t prefix_len = 0;

    switch (type) {
    case 'd':
        base = 10;
        break;
    case 'x':
        base = 16;
        prefix = "0x";
        prefix_len = (spec.alternate && value != 0) ? 2u : 0u;
        break;
    case 'X':
        base = 16;
        uppercase = true;
        prefix = "0X";
        prefix_len = (spec.alternate && value != 0) ? 2u : 0u;
        break;
    case 'b':
        base = 2;
        prefix = "0b";
        prefix_len = spec.alternate ? 2u : 0u;
        break;
    case 'B':
        base = 2;
        uppercase = true;
        prefix = "0B";
        prefix_len = spec.alternate ? 2u : 0u;
        break;
    case 'o':
        base = 8;
        prefix = "0";
        prefix_len = (spec.alternate && value != 0) ? 1u : 0u;
        break;
    default:
        ctx.fail(errc::bad_spec);
        return false;
    }

    number_layout layout{};
    layout.prefix = prefix;
    layout.prefix_len = prefix_len;
    layout.digits = make_digits(value, base, uppercase);

    if (negative) {
        layout.sign_char = '-';
    } else if (spec.sign_mode == sign::plus) {
        layout.sign_char = '+';
    } else if (spec.sign_mode == sign::space) {
        layout.sign_char = ' ';
    }

    return write_number_layout(ctx, layout, spec);
}

template<typename Ctx>
constexpr bool write_pointer(Ctx& ctx, const void* ptr, const format_spec& spec) noexcept {
    if (spec.type != 0 && spec.type != 'p') {
        ctx.fail(errc::bad_spec);
        return false;
    }
    if (spec.sign_mode != sign::minus_only || spec.alternate || spec.has_precision) {
        ctx.fail(errc::bad_spec);
        return false;
    }

    format_spec numeric_spec = spec;
    numeric_spec.type = 'x';
    numeric_spec.alternate = false;

    number_layout layout{};
    layout.prefix = "0x";
    layout.prefix_len = 2;
    layout.digits = make_digits(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ptr)), 16, false);
    return write_number_layout(ctx, layout, numeric_spec);
}

template<typename R>
constexpr bool custom_result_ok(R value) noexcept {
    if constexpr (SameAs<R, bool>) {
        return value;
    } else if constexpr (SameAs<R, errc>) {
        return value == errc::ok;
    } else if constexpr (SameAs<R, result>) {
        return value.ok();
    } else {
        return static_cast<bool>(value);
    }
}

template<typename Sink, typename T>
constexpr bool format_custom(output_context<Sink>& ctx, const format_spec& spec, const T& value) noexcept {
    formatter<T> f{};
    using R = decltype(f.format(value, ctx, spec));
    if constexpr (is_void_v<R>) {
        f.format(value, ctx, spec);
        return true;
    } else {
        const R r = f.format(value, ctx, spec);
        if (!custom_result_ok(r)) {
            if (ctx.error() == errc::ok) {
                ctx.fail(errc::bad_spec);
            }
            return false;
        }
        return true;
    }
}

template<typename Sink, typename T>
constexpr bool format_value(output_context<Sink>& ctx, const format_spec& spec, const T& value) noexcept {
    using Raw = remove_ref_t<T>;
    using Clean = remove_cvr_t<T>;

    if constexpr (has_custom_formatter<Clean, output_context<Sink>>) {
        return format_custom(ctx, spec, static_cast<const Clean&>(value));
    } else if constexpr (is_char_array<Raw>::value || is_char_array<Clean>::value) {
        size_t n = 0;
        while (n < sizeof(value) && value[n] != '\0') {
            ++n;
        }
        return write_string(ctx, StrView{value, n}, spec);
    } else if constexpr (SameAs<Clean, StrView>) {
        return write_string(ctx, value, spec);
    } else if constexpr (SameAs<Clean, bool>) {
        if (spec.type == 0 || spec.type == 's' || spec.type == '?') {
            return write_string(ctx, value ? StrView{"true", 4} : StrView{"false", 5}, spec);
        }
        return write_unsigned_number(ctx, value ? 1u : 0u, spec, false);
    } else if constexpr (SameAs<Clean, char>) {
        if (spec.type == 0 || spec.type == 'c' || spec.type == '?') {
            return write_char(ctx, value, spec);
        }
        return write_unsigned_number(ctx, static_cast<unsigned char>(value), spec, false);
    } else if constexpr (is_c_string_pointer<Clean>::value) {
        if (spec.type == 'p') {
            return write_pointer(ctx, static_cast<const void*>(value), spec);
        }
        if (value == nullptr) {
            return write_string(ctx, StrView{"(null)", 6}, spec);
        }
        return write_string(ctx, StrView{value, cstr_len(value)}, spec);
    } else if constexpr (is_signed_integer_v<Clean>) {
        const int64_t signed_value = static_cast<int64_t>(value);
        return write_unsigned_number(ctx, signed_abs_to_u64(signed_value), spec, signed_value < 0);
    } else if constexpr (is_unsigned_integer_v<Clean>) {
        return write_unsigned_number(ctx, static_cast<uint64_t>(value), spec, false);
    } else if constexpr (__is_enum(Clean)) {
        using Underlying = __underlying_type(Clean);
        return format_value(ctx, spec, static_cast<Underlying>(value));
    } else if constexpr (is_pointer_v<Clean>) {
        return write_pointer(ctx, static_cast<const void*>(value), spec);
    } else if constexpr (SameAs<Clean, decltype(nullptr)>) {
        return write_pointer(ctx, nullptr, spec);
    } else {
        ctx.fail(errc::type_mismatch);
        return false;
    }
}

template<size_t I, typename Sink>
constexpr bool format_indexed_arg(output_context<Sink>& ctx, size_t, const format_spec&) noexcept {
    ctx.fail(errc::arg_missing);
    return false;
}

template<size_t I, typename Sink, typename First, typename... Rest>
constexpr bool format_indexed_arg(output_context<Sink>& ctx, size_t target,
    const format_spec& spec, const First& first, const Rest&... rest) noexcept {
    if (target == I) {
        return format_value(ctx, spec, first);
    }
    if constexpr (sizeof...(Rest) == 0) {
        ctx.fail(errc::arg_missing);
        return false;
    } else {
        return format_indexed_arg<I + 1>(ctx, target, spec, rest...);
    }
}

template<typename Sink, typename... Args>
constexpr result vformat_to(Sink& sink, StrView fmt_text, const Args&... args) noexcept {
    output_context<Sink> ctx{sink};
    size_t next_auto_index = 0;
    enum class index_mode : uint8_t { none, automatic, manual };
    index_mode mode = index_mode::none;

    for (size_t pos = 0; pos < fmt_text.size();) {
        const char c = fmt_text[pos];
        if (c == '{') {
            if ((pos + 1) < fmt_text.size() && fmt_text[pos + 1] == '{') {
                if (!ctx.put('{')) return ctx.finish();
                pos += 2;
                continue;
            }

            ++pos;
            replacement_field field{};
            const errc parse_error = parse_replacement_field(fmt_text, pos, field);
            if (parse_error != errc::ok) {
                ctx.fail(parse_error);
                return ctx.finish();
            }

            size_t index = field.index;
            if (field.has_index) {
                if (mode == index_mode::automatic) {
                    ctx.fail(errc::bad_format);
                    return ctx.finish();
                }
                mode = index_mode::manual;
            } else {
                if (mode == index_mode::manual) {
                    ctx.fail(errc::bad_format);
                    return ctx.finish();
                }
                mode = index_mode::automatic;
                index = next_auto_index++;
            }

            if (!format_indexed_arg<0>(ctx, index, field.spec, args...)) {
                return ctx.finish();
            }
            continue;
        }

        if (c == '}') {
            if ((pos + 1) < fmt_text.size() && fmt_text[pos + 1] == '}') {
                if (!ctx.put('}')) return ctx.finish();
                pos += 2;
                continue;
            }
            ctx.fail(errc::bad_format);
            return ctx.finish();
        }

        const size_t start = pos;
        while (pos < fmt_text.size() && fmt_text[pos] != '{' && fmt_text[pos] != '}') {
            ++pos;
        }
        if (!ctx.write(fmt_text.data() + start, pos - start)) {
            return ctx.finish();
        }
    }

    return ctx.finish();
}

template<size_t N>
struct fixed_string {
    char value[N]{};

    constexpr fixed_string(const char (&text)[N]) noexcept {
        for (size_t i = 0; i < N; ++i) {
            value[i] = text[i];
        }
    }

    constexpr const char* data() const noexcept { return value; }
    constexpr size_t size() const noexcept { return N > 0 ? N - 1 : 0; }
};

template<size_t N>
fixed_string(const char (&)[N]) -> fixed_string<N>;

template<fixed_string F, size_t ArgCount>
consteval bool static_format_valid() noexcept {
    size_t pos = 0;
    size_t automatic_count = 0;
    size_t max_manual_index = 0;
    bool has_manual = false;
    bool has_automatic = false;

    while (pos < F.size()) {
        const char c = F.value[pos];
        if (c == '{') {
            if ((pos + 1) < F.size() && F.value[pos + 1] == '{') {
                pos += 2;
                continue;
            }
            ++pos;

            bool has_index = false;
            size_t index = 0;
            if (pos < F.size() && is_digit(F.value[pos])) {
                has_index = true;
                while (pos < F.size() && is_digit(F.value[pos])) {
                    if (!checked_add_digit(index, F.value[pos])) {
                        return false;
                    }
                    ++pos;
                }
            }

            if (pos < F.size() && F.value[pos] == ':') {
                ++pos;
                if ((pos + 1) < F.size() && is_align_char(F.value[pos + 1])) {
                    pos += 2;
                } else if (pos < F.size() && is_align_char(F.value[pos])) {
                    ++pos;
                }
                if (pos < F.size() && (F.value[pos] == '+' || F.value[pos] == '-' || F.value[pos] == ' ')) ++pos;
                if (pos < F.size() && F.value[pos] == '#') ++pos;
                if (pos < F.size() && F.value[pos] == '0') ++pos;
                while (pos < F.size() && is_digit(F.value[pos])) ++pos;
                if (pos < F.size() && F.value[pos] == '.') {
                    ++pos;
                    if (pos >= F.size() || !is_digit(F.value[pos])) return false;
                    while (pos < F.size() && is_digit(F.value[pos])) ++pos;
                }
                if (pos < F.size() && F.value[pos] != '}') ++pos;
            }

            if (pos >= F.size() || F.value[pos] != '}') {
                return false;
            }
            ++pos;

            if (has_index) {
                has_manual = true;
                if (index > max_manual_index) {
                    max_manual_index = index;
                }
            } else {
                has_automatic = true;
                ++automatic_count;
            }
            if (has_manual && has_automatic) {
                return false;
            }
            continue;
        }

        if (c == '}') {
            if ((pos + 1) < F.size() && F.value[pos + 1] == '}') {
                pos += 2;
                continue;
            }
            return false;
        }
        ++pos;
    }

    if (has_manual) {
        return max_manual_index < ArgCount;
    }
    return automatic_count <= ArgCount;
}

} // namespace detail

using detail::fixed_string;

template<typename Sink, typename... Args>
constexpr result format_to(Sink& sink, StrView fmt_text, const Args&... args) noexcept {
    return detail::vformat_to(sink, fmt_text, args...);
}

template<typename Sink, size_t N, typename... Args>
constexpr result format_to(Sink& sink, const char (&fmt_text)[N], const Args&... args) noexcept {
    return detail::vformat_to(sink, StrView{fmt_text, N - 1}, args...);
}

template<typename Sink, typename... Args>
constexpr result format_cstr_to(Sink& sink, const char* fmt_text, const Args&... args) noexcept {
    return detail::vformat_to(sink, StrView::from_cstr(fmt_text), args...);
}

template<detail::fixed_string F, typename Sink, typename... Args>
constexpr result format_to(Sink& sink, const Args&... args) noexcept {
    static_assert(detail::static_format_valid<F, sizeof...(Args)>(),
        "libk::fmt checked format string is malformed or references a missing argument");
    return detail::vformat_to(sink, StrView{F.data(), F.size()}, args...);
}

template<typename... Args>
constexpr result format_to_n(char* dst, size_t capacity, StrView fmt_text, const Args&... args) noexcept {
    span_buffer buffer{dst, capacity};
    result r = format_to(buffer, fmt_text, args...);
    if (!buffer.null_terminate() && r.error == errc::ok) {
        r.error = errc::output_truncated;
    }
    return r;
}

template<size_t N, typename... Args>
constexpr result format_to_n(char (&dst)[N], StrView fmt_text, const Args&... args) noexcept {
    return format_to_n(dst, N, fmt_text, args...);
}

template<size_t N, size_t M, typename... Args>
constexpr result format_to_n(char (&dst)[N], const char (&fmt_text)[M], const Args&... args) noexcept {
    return format_to_n(dst, N, StrView{fmt_text, M - 1}, args...);
}

template<typename... Args>
constexpr result format_cstr_to_n(char* dst, size_t capacity, const char* fmt_text, const Args&... args) noexcept {
    return format_to_n(dst, capacity, StrView::from_cstr(fmt_text), args...);
}

template<typename... Args>
constexpr result formatted_size(StrView fmt_text, const Args&... args) noexcept {
    counting_sink sink{};
    return format_to(sink, fmt_text, args...);
}

template<size_t N, typename... Args>
constexpr result formatted_size(const char (&fmt_text)[N], const Args&... args) noexcept {
    counting_sink sink{};
    return format_to(sink, fmt_text, args...);
}

} // namespace libk::fmt
