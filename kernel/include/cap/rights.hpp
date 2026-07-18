#pragma once

#include <core/types.hpp>
#include <libk/concepts.hpp>
#include <libk/optional.hpp>

namespace kernel::cap {

enum class Right : u64 {
    Duplicate = u64{1} << 0,
    Delegate = u64{1} << 1,
    Reserve = u64{1} << 2,
    CreateRegion = u64{1} << 3,
    Map = u64{1} << 4,
    Unmap = u64{1} << 5,
    Protect = u64{1} << 6,
    Destroy = u64{1} << 7,
    Inspect = u64{1} << 8,
    Control = u64{1} << 9,
    Manage = u64{1} << 10,
    Revoke = u64{1} << 11,
    Create = u64{1} << 12,
    Split = u64{1} << 13,
    Close = u64{1} << 14,
    Signal = u64{1} << 15,
    Wait = u64{1} << 16,
    Connect = u64{1} << 17,
    Ack = u64{1} << 18,
};

class Rights final {
public:
    constexpr Rights() noexcept = default;

    template<typename... R>
        requires((libk::SameAs<R, Right> && ...))
    [[nodiscard]] static constexpr auto of(R... rights) noexcept -> Rights {
        return Rights{(u64{} | ... | static_cast<u64>(rights))};
    }

    [[nodiscard]] constexpr auto contains(Right right) const noexcept -> bool {
        const u64 bit = static_cast<u64>(right);
        return (bits_ & bit) == bit;
    }
    [[nodiscard]] constexpr auto contains(Rights rights) const noexcept -> bool {
        return (bits_ & rights.bits_) == rights.bits_;
    }
    [[nodiscard]] constexpr auto intersect(Rights other) const noexcept
        -> Rights {
        return Rights{bits_ & other.bits_};
    }
    [[nodiscard]] constexpr auto empty() const noexcept -> bool {
        return bits_ == 0;
    }
    [[nodiscard]] constexpr auto raw() const noexcept -> u64 { return bits_; }
    [[nodiscard]] static constexpr auto from_raw(u64 bits) noexcept
        -> libk::optional<Rights> {
        constexpr u64 valid = (u64{1} << 19) - 1;
        return (bits & ~valid) == 0
            ? libk::optional<Rights>{Rights{bits}}
            : libk::nullopt;
    }

    [[nodiscard]] friend constexpr auto operator==(
        Rights, Rights) noexcept -> bool = default;

private:
    explicit constexpr Rights(u64 bits) noexcept : bits_(bits) {}
    u64 bits_{};
};

} // namespace kernel::cap
