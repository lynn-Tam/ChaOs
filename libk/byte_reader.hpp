#pragma once

#include <stddef.h>
#include <stdint.h>

#include <libk/assert.hpp>
#include <bit>
#include <array>
#include <concepts>
#include <libk/checked_arithmetic.hpp>
#include <span>
#include <string_view>

namespace libk {

// Bounds-checked forward reader. Failed operations never advance the cursor or
// modify an output value. Empty input may use a null base; non-empty input may
// not, so no null-pointer arithmetic is ever performed.
class byte_reader final {
public:
    constexpr byte_reader(const uint8_t* base, size_t size) noexcept
        : base_(base), size_(size) {
        libk_assert(base_ != nullptr || size_ == 0);
    }

    [[nodiscard]] constexpr auto ptr() const noexcept -> const uint8_t* {
        return base_ == nullptr ? nullptr : base_ + offset_;
    }
    [[nodiscard]] constexpr auto remaining() const noexcept -> size_t {
        return size_ - offset_;
    }
    [[nodiscard]] constexpr auto offset() const noexcept -> size_t {
        return offset_;
    }

    [[nodiscard]] constexpr auto skip(size_t count) noexcept -> bool {
        if (count > remaining()) {
            return false;
        }
        offset_ += count;
        return true;
    }

    [[nodiscard]] auto align(size_t alignment) noexcept -> bool {
        if (base_ == nullptr) {
            return std::has_single_bit(alignment) && remaining() == 0;
        }
        const uintptr_t current =
            reinterpret_cast<uintptr_t>(base_ + offset_);
        const auto aligned = checked_align_up(current, alignment);
        if (!aligned) {
            return false;
        }
        return skip(static_cast<size_t>(*aligned - current));
    }

    template<std::unsigned_integral T>
    [[nodiscard]] constexpr auto read_be(T& output) noexcept -> bool {
        return read<std::endian::big>(output);
    }
    template<std::unsigned_integral T>
    [[nodiscard]] constexpr auto read_le(T& output) noexcept -> bool {
        return read<std::endian::little>(output);
    }

    [[nodiscard]] constexpr auto take_bytes(
        size_t count,
        std::span<const uint8_t>& output) noexcept -> bool {
        if (count > remaining()) {
            return false;
        }
        output = std::span<const uint8_t>{ptr(), count};
        offset_ += count;
        return true;
    }

    [[nodiscard]] constexpr auto read_cstr(std::string_view& output) noexcept -> bool {
        size_t length{};
        while (length < remaining()
               && base_[offset_ + length] != uint8_t{}) {
            ++length;
        }
        if (length == remaining()) {
            return false;
        }
        output = std::string_view{
            reinterpret_cast<const char*>(base_ + offset_), length};
        offset_ += length + 1;
        return true;
    }

private:
    template<std::endian Order, std::unsigned_integral T>
        requires (!std::same_as<T, bool>)
    constexpr auto read(T& output) noexcept -> bool {
        if (remaining() < sizeof(T)) return false;
        std::array<uint8_t, sizeof(T)> bytes;
        for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = base_[offset_ + i];
        auto value = std::bit_cast<T>(bytes);
        if constexpr (Order != std::endian::native) value = std::byteswap(value);
        output = value;
        offset_ += sizeof(T);
        return true;
    }
    const uint8_t* base_{};
    size_t size_{};
    size_t offset_{};
};

} // namespace libk
