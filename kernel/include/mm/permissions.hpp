#pragma once

#include <core/types.hpp>
#include <libk/concepts.hpp>

namespace kernel::mm {

enum class MemoryType : u8 {
    Normal,
    Uncached,
    Device,
};

class MemoryTypes final {
public:
    constexpr MemoryTypes() noexcept = default;

    template<typename... T>
        requires((libk::SameAs<T, MemoryType> && ...))
    [[nodiscard]] static constexpr auto of(T... types) noexcept
        -> MemoryTypes {
        return MemoryTypes{static_cast<u8>(
            (u8{} | ... | (u8{1} << static_cast<u8>(types))))};
    }

    [[nodiscard]] constexpr auto contains(MemoryType type) const noexcept
        -> bool {
        const u8 bit = static_cast<u8>(u8{1} << static_cast<u8>(type));
        return (bits_ & bit) == bit;
    }
    [[nodiscard]] constexpr auto contains(MemoryTypes other) const noexcept
        -> bool {
        return (bits_ & other.bits_) == other.bits_;
    }
    [[nodiscard]] constexpr auto intersect(MemoryTypes other) const noexcept
        -> MemoryTypes {
        return MemoryTypes{static_cast<u8>(bits_ & other.bits_)};
    }
    [[nodiscard]] constexpr auto empty() const noexcept -> bool {
        return bits_ == 0;
    }
    [[nodiscard]] constexpr auto raw() const noexcept -> u8 { return bits_; }
    [[nodiscard]] static constexpr auto from_raw(u8 bits) noexcept
        -> MemoryTypes { return MemoryTypes{bits}; }
    [[nodiscard]] friend constexpr auto operator==(
        MemoryTypes, MemoryTypes) noexcept -> bool = default;

private:
    explicit constexpr MemoryTypes(u8 bits) noexcept : bits_(bits) {}
    u8 bits_{};
};

[[nodiscard]] constexpr auto valid_memory_types(
    MemoryTypes types) noexcept -> bool {
    constexpr u8 valid = (u8{1} << static_cast<u8>(MemoryType::Normal))
        | (u8{1} << static_cast<u8>(MemoryType::Uncached))
        | (u8{1} << static_cast<u8>(MemoryType::Device));
    return !types.empty() && (types.raw() & ~valid) == 0;
}

enum class Access : u8 {
    Read = u8{1} << 0,
    Write = u8{1} << 1,
    Execute = u8{1} << 2,
};

class AccessMask final {
public:
    constexpr AccessMask() noexcept = default;

    template<typename... A>
        requires((libk::SameAs<A, Access> && ...))
    [[nodiscard]] static constexpr auto of(A... access) noexcept
        -> AccessMask {
        return AccessMask{static_cast<u8>(
            (u8{} | ... | static_cast<u8>(access)))};
    }

    [[nodiscard]] constexpr auto contains(Access access) const noexcept
        -> bool {
        const u8 bit = static_cast<u8>(access);
        return (bits_ & bit) == bit;
    }
    [[nodiscard]] constexpr auto contains(AccessMask other) const noexcept
        -> bool {
        return (bits_ & other.bits_) == other.bits_;
    }
    [[nodiscard]] constexpr auto intersect(AccessMask other) const noexcept
        -> AccessMask {
        return AccessMask{static_cast<u8>(bits_ & other.bits_)};
    }
    [[nodiscard]] constexpr auto empty() const noexcept -> bool {
        return bits_ == 0;
    }
    [[nodiscard]] constexpr auto raw() const noexcept -> u8 { return bits_; }
    [[nodiscard]] static constexpr auto from_raw(u8 bits) noexcept
        -> AccessMask { return AccessMask{bits}; }
    [[nodiscard]] friend constexpr auto operator==(
        AccessMask, AccessMask) noexcept -> bool = default;

private:
    explicit constexpr AccessMask(u8 bits) noexcept : bits_(bits) {}
    u8 bits_{};
};

[[nodiscard]] constexpr auto valid_access(AccessMask access) noexcept -> bool {
    constexpr u8 valid = static_cast<u8>(Access::Read)
        | static_cast<u8>(Access::Write)
        | static_cast<u8>(Access::Execute);
    return !access.empty()
        && (access.raw() & ~valid) == 0
        && (!access.contains(Access::Write)
            || access.contains(Access::Read));
}

} // namespace kernel::mm
