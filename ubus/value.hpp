#pragma once
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string.h>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// Every peer uses this encoding, including method descriptions. Integers and
// floating values have canonical 64-bit payloads; arrays may contain any value.
enum class ubus_type : uint16_t { Null, Bool, I64, U64, F64, String, Bytes, Array, Record, Cap, Buffer, Vector };
enum class ubus_atom : uint16_t { U8, I8, U16, I16, U32, I32, U64, I64, F32, F64 };
struct ubus_vector { ubus_atom atom; uint16_t reserved; uint32_t count; };
template<class T> constexpr auto ubus_atom_of() -> ubus_atom {
    static_assert((std::integral<T> && !std::same_as<T, bool>) || std::floating_point<T>);
    if constexpr (std::floating_point<T>) {
        static_assert(sizeof(T) == 4 || sizeof(T) == 8);
        return sizeof(T) == 4 ? ubus_atom::F32 : ubus_atom::F64;
    } else return static_cast<ubus_atom>(std::countr_zero(sizeof(T)) * 2 + std::is_signed_v<T>);
}
struct ubus_buffer { uint64_t cap{}, offset{}, size{}; };
struct ubus_head { uint32_t bytes; ubus_type type; uint16_t name; };
inline constexpr size_t ubus_depth = 32;
static_assert(sizeof(ubus_head) == 8 && std::endian::native == std::endian::little);
constexpr auto ubus_align(size_t size) noexcept -> size_t { return (size + 7) & ~size_t{7}; }

class ubus_value;
struct ubus_range {
    std::span<const std::byte> bytes;
    struct iterator {
        using value_type = ubus_value;
        using difference_type = ptrdiff_t;
        using iterator_category = std::input_iterator_tag;
        std::span<const std::byte> bytes;
        auto operator*() const noexcept -> ubus_value;
        auto operator++() noexcept -> iterator&;
        void operator++(int) noexcept { ++*this; }
        auto operator==(std::default_sentinel_t) const noexcept -> bool { return bytes.empty(); }
    };
    auto begin() const noexcept -> iterator { return {bytes}; }
    auto end() const noexcept -> std::default_sentinel_t { return {}; }
};

// A view borrows one received packet or builder. No field owns a heap tree.
class ubus_value {
    std::span<const std::byte> bytes_{};
    ubus_head head_{};
public:
    ubus_value() = default;
    explicit ubus_value(std::span<const std::byte> bytes) noexcept;
    explicit operator bool() const noexcept { return !bytes_.empty(); }
    auto type() const noexcept -> ubus_type { return head_.type; }
    auto name() const noexcept -> std::string_view {
        return *this ? std::string_view{reinterpret_cast<const char*>(bytes_.data() + sizeof(head_)), head_.name}
                     : std::string_view{};
    }
    auto data() const noexcept -> std::span<const std::byte> {
        return *this ? bytes_.subspan(sizeof(head_) + ubus_align(head_.name)) : std::span<const std::byte>{};
    }
    auto bytes() const noexcept -> std::span<const std::byte> { return bytes_; }
    auto values() const noexcept -> ubus_range {
        return {type() == ubus_type::Array || type() == ubus_type::Record ? data() : std::span<const std::byte>{}};
    }
    auto operator[](std::string_view name) const noexcept -> ubus_value;
    auto operator[](size_t index) const noexcept -> ubus_value;
    auto valid(size_t depth = ubus_depth) const noexcept -> bool;
    template<class T> auto vector() const noexcept -> std::optional<std::span<const T>> {
        if (type() != ubus_type::Vector || data().size() < sizeof(ubus_vector)) return {};
        ubus_vector head; ::memcpy(&head, data().data(), sizeof(head));
        const auto payload = data().subspan(sizeof(head));
        if (head.atom != ubus_atom_of<T>() || head.reserved || payload.size() / sizeof(T) != head.count
            || payload.size() % sizeof(T) || reinterpret_cast<uintptr_t>(payload.data()) % alignof(T)) return {};
        return std::span{reinterpret_cast<const T*>(payload.data()), static_cast<size_t>(head.count)};
    }
    template<class T> auto as() const noexcept -> std::optional<T> {
        const auto payload = data();
        if constexpr (std::same_as<T, std::nullptr_t>) {
            if (*this && type() == ubus_type::Null && payload.empty()) return nullptr;
        } else if constexpr (std::same_as<T, std::string_view>) {
            if (*this && type() == ubus_type::String)
                return std::string_view{reinterpret_cast<const char*>(payload.data()), payload.size()};
        } else if constexpr (std::same_as<T, std::span<const std::byte>>) {
            if (*this && type() == ubus_type::Bytes) return payload;
        } else if constexpr (std::same_as<T, ubus_buffer>) {
            if (*this && type() == ubus_type::Buffer && payload.size() == sizeof(T)) {
                T value; ::memcpy(&value, payload.data(), sizeof(value)); return value;
            }
        } else if constexpr (std::integral<T> || std::floating_point<T>) {
            if (payload.size() != 8) return {};
            uint64_t bits; ::memcpy(&bits, payload.data(), 8);
            if constexpr (std::same_as<T, bool>) {
                if (type() == ubus_type::Bool && bits <= 1) return bits != 0;
            } else if constexpr (std::integral<T>) {
                if (type() == ubus_type::U64 && std::in_range<T>(bits)) return static_cast<T>(bits);
                const auto value = std::bit_cast<int64_t>(bits);
                if (type() == ubus_type::I64 && std::in_range<T>(value)) return static_cast<T>(value);
            } else {
                if (type() == ubus_type::F64) return static_cast<T>(std::bit_cast<double>(bits));
            }
        }
        return {};
    }
};

// One owning, contiguous encoding. put/push share append; nested values remain
// ordinary encoded containers rather than interface-specific C++ layouts.
class ubus_data {
    static inline constexpr ubus_head empty_{sizeof(ubus_head), ubus_type::Record, 0};
    std::vector<std::byte> bytes_;
    bool failed_{};
    void append(std::string_view name, ubus_type type, std::span<const std::byte> data,
                std::span<const std::byte> prefix = {});
    template<class T> void scalar(std::string_view name, ubus_type type, const T& value) {
        append(name, type, std::as_bytes(std::span{&value, size_t{1}}));
    }
public:
    ubus_data() = default;
    explicit ubus_data(ubus_type type);
    auto view() const noexcept -> ubus_value { return ubus_value{bytes()}; }
    auto operator[](std::string_view name) const noexcept -> ubus_value { return view()[name]; }
    auto bytes() const noexcept -> std::span<const std::byte> {
        return bytes_.empty() ? std::as_bytes(std::span{&empty_, size_t{1}}) : std::span<const std::byte>{bytes_};
    }
    auto valid() const noexcept -> bool { return !failed_ && view().bytes().size() == bytes().size() && view().valid(); }
    auto assign(std::span<const std::byte> bytes) -> bool;
    void reserve(size_t size) { bytes_.reserve(size); }
    auto storage(size_t size) -> std::span<std::byte> { bytes_.resize(size); failed_ = false; return bytes_; }
    template<class T, size_t Extent> auto array(std::string_view name, std::span<T, Extent> values) -> ubus_data& {
        if (values.size() > UINT32_MAX) { failed_ = true; return *this; }
        const ubus_vector head{ubus_atom_of<std::remove_cv_t<T>>(), 0, static_cast<uint32_t>(values.size())};
        append(name, ubus_type::Vector, std::as_bytes(values), std::as_bytes(std::span{&head, size_t{1}}));
        return *this;
    }
    template<class T> auto put(std::string_view name, const T& value) -> ubus_data& {
        using V = std::remove_cvref_t<T>;
        if constexpr (std::same_as<V, ubus_data>) {
            if (value.failed_) failed_ = true;
            return put(name, value.view());
        }
        else if constexpr (std::same_as<V, ubus_value>) {
            if (value) append(name, value.type(), value.data()); else failed_ = true;
        } else if constexpr (std::same_as<V, std::nullptr_t>) append(name, ubus_type::Null, {});
        else if constexpr (std::same_as<V, ubus_buffer>) scalar(name, ubus_type::Buffer, value);
        else if constexpr (std::same_as<V, bool>) scalar(name, ubus_type::Bool, uint64_t{value});
        else if constexpr (std::integral<V>) {
            if constexpr (std::is_signed_v<V>) scalar(name, ubus_type::I64, static_cast<int64_t>(value));
            else scalar(name, ubus_type::U64, static_cast<uint64_t>(value));
        } else if constexpr (std::floating_point<V>) scalar(name, ubus_type::F64, static_cast<double>(value));
        else if constexpr (std::convertible_to<const T&, std::string_view>) {
            const std::string_view text{value}; append(name, ubus_type::String, std::as_bytes(std::span{text}));
        } else if constexpr (std::convertible_to<const T&, std::span<const std::byte>>) {
            append(name, ubus_type::Bytes, std::span<const std::byte>{value});
        } else static_assert(sizeof(T) == 0, "unsupported ubus value");
        return *this;
    }
    template<class T> auto push(const T& value) -> ubus_data& { return put({}, value); }
    auto cap(std::string_view name, uint64_t index) -> ubus_data& {
        scalar(name, ubus_type::Cap, index); return *this;
    }
};
