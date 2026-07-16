#pragma once

#include <core/types.hpp>

namespace kernel::cap {

class CapHandle final {
public:
    static constexpr usize index_bits = 24;
    static constexpr usize generation_bits = 64 - index_bits;
    static constexpr u64 max_index = (u64{1} << index_bits) - 1;
    static constexpr u64 max_generation =
        (u64{1} << generation_bits) - 1;

    constexpr CapHandle() noexcept = default;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return value_ != 0;
    }
    [[nodiscard]] constexpr auto raw() const noexcept -> u64 {
        return value_;
    }
    [[nodiscard]] constexpr auto index() const noexcept -> usize {
        return static_cast<usize>(value_ & max_index);
    }
    [[nodiscard]] constexpr auto generation() const noexcept -> u64 {
        return value_ >> index_bits;
    }

    [[nodiscard]] static constexpr auto from_raw(u64 value) noexcept
        -> CapHandle {
        const CapHandle handle{value};
        return handle.generation() != 0 && handle.index() <= max_index
            ? handle
            : CapHandle{};
    }

    [[nodiscard]] friend constexpr auto operator==(
        CapHandle, CapHandle) noexcept -> bool = default;

private:
    friend class CSpace;

    [[nodiscard]] static constexpr auto make(
        usize index,
        u64 generation) noexcept -> CapHandle {
        return index <= max_index
                && generation != 0
                && generation <= max_generation
            ? CapHandle{(generation << index_bits) | index}
            : CapHandle{};
    }

    explicit constexpr CapHandle(u64 value) noexcept : value_(value) {}
    u64 value_{};
};

static_assert(sizeof(CapHandle) == sizeof(u64));
static_assert(!CapHandle{});

} // namespace kernel::cap
