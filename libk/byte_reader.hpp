#pragma once

#include <stddef.h>
#include <stdint.h>

#include <libk/assert.hpp>
#include <libk/bits.hpp>
#include <libk/checked_arithmetic.hpp>
#include <libk/span.hpp>
#include <libk/string_view.hpp>

namespace libk {

// Bounds-checked forward reader. Failed operations never advance the cursor or
// modify an output value. Empty input may use a null base; non-empty input may
// not, so no null-pointer arithmetic is ever performed.
class ByteReader final {
public:
    constexpr ByteReader(const uint8_t* base, size_t size) noexcept
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
            return has_single_bit(alignment) && remaining() == 0;
        }
        const uintptr_t current =
            reinterpret_cast<uintptr_t>(base_ + offset_);
        const auto aligned = checked_align_up(current, alignment);
        if (!aligned) {
            return false;
        }
        return skip(static_cast<size_t>(*aligned - current));
    }

    [[nodiscard]] constexpr auto read_be32(uint32_t& output) noexcept -> bool {
        if (remaining() < sizeof(uint32_t)) {
            return false;
        }
        const uint8_t* const current = base_ + offset_;
        const uint32_t value =
            static_cast<uint32_t>(current[0]) << 24
            | static_cast<uint32_t>(current[1]) << 16
            | static_cast<uint32_t>(current[2]) << 8
            | static_cast<uint32_t>(current[3]);
        offset_ += sizeof(uint32_t);
        output = value;
        return true;
    }

    [[nodiscard]] constexpr auto read_be64(uint64_t& output) noexcept -> bool {
        if (remaining() < sizeof(uint64_t)) {
            return false;
        }
        const uint8_t* const current = base_ + offset_;
        uint64_t value{};
        for (size_t index = 0; index < sizeof(uint64_t); ++index) {
            value = (value << 8) | current[index];
        }
        offset_ += sizeof(uint64_t);
        output = value;
        return true;
    }

    [[nodiscard]] constexpr auto read_le16(uint16_t& output) noexcept -> bool {
        if (remaining() < sizeof(uint16_t)) {
            return false;
        }
        const uint8_t* const current = base_ + offset_;
        output = static_cast<uint16_t>(current[0])
            | static_cast<uint16_t>(current[1]) << 8;
        offset_ += sizeof(uint16_t);
        return true;
    }

    [[nodiscard]] constexpr auto read_le32(uint32_t& output) noexcept -> bool {
        if (remaining() < sizeof(uint32_t)) {
            return false;
        }
        const uint8_t* const current = base_ + offset_;
        output = static_cast<uint32_t>(current[0])
            | static_cast<uint32_t>(current[1]) << 8
            | static_cast<uint32_t>(current[2]) << 16
            | static_cast<uint32_t>(current[3]) << 24;
        offset_ += sizeof(uint32_t);
        return true;
    }

    [[nodiscard]] constexpr auto read_le64(uint64_t& output) noexcept -> bool {
        if (remaining() < sizeof(uint64_t)) {
            return false;
        }
        const uint8_t* const current = base_ + offset_;
        uint64_t value{};
        for (size_t index = 0; index < sizeof(uint64_t); ++index) {
            value |= static_cast<uint64_t>(current[index]) << (index * 8);
        }
        offset_ += sizeof(uint64_t);
        output = value;
        return true;
    }

    [[nodiscard]] constexpr auto take_bytes(
        size_t count,
        ByteSpan& output) noexcept -> bool {
        if (count > remaining()) {
            return false;
        }
        output = ByteSpan{ptr(), count};
        offset_ += count;
        return true;
    }

    [[nodiscard]] constexpr auto read_cstr(StrView& output) noexcept -> bool {
        size_t length{};
        while (length < remaining()
               && base_[offset_ + length] != uint8_t{}) {
            ++length;
        }
        if (length == remaining()) {
            return false;
        }
        output = StrView{
            reinterpret_cast<const char*>(base_ + offset_), length};
        offset_ += length + 1;
        return true;
    }

private:
    const uint8_t* base_{};
    size_t size_{};
    size_t offset_{};
};

} // namespace libk
