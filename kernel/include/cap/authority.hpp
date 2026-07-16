#pragma once

#include <cap/rights.hpp>
#include <libk/variant.hpp>
#include <mm/addr.hpp>
#include <mm/object_range.hpp>
#include <mm/permissions.hpp>
#include <mm/vm_key.hpp>

namespace kernel::mm {
class VSpace;
}

namespace kernel::cap {

struct MemoryAuthority final {
    kernel::mm::ObjectRange range{};
    kernel::mm::AccessMask access{};
    kernel::mm::MemoryTypes types{};

    [[nodiscard]] friend constexpr auto operator==(
        MemoryAuthority, MemoryAuthority) noexcept -> bool = default;
};

struct VSpaceAuthority final {
    kernel::mm::RegionKey region{};
    kernel::mm::VirtRange range{};
    kernel::mm::AccessMask access{};
    kernel::mm::MemoryTypes types{};

    [[nodiscard]] friend constexpr auto operator==(
        VSpaceAuthority, VSpaceAuthority) noexcept -> bool = default;
};

using AuthorityData = libk::variant<
    libk::monostate,
    MemoryAuthority,
    VSpaceAuthority>;

struct GrantCeiling final {
    Rights rights{};
    AuthorityData data{};
};

struct CapView final {
    Rights rights{};
    AuthorityData data{};
};

struct EffectiveAuthority final {
    Rights rights{};
    AuthorityData data{};

    [[nodiscard]] auto ceiling() const noexcept -> GrantCeiling {
        return GrantCeiling{rights, data};
    }
};

// Proof carried only across the VSpace-owned create-region transaction. It
// authorizes one resource-identity change that generic duplicate/delegate
// must reject; GrantGraph still validates all immutable proof fields.
class RegionDerivation final {
public:
    [[nodiscard]] auto parent() const noexcept -> kernel::mm::RegionKey {
        return parent_;
    }
    [[nodiscard]] auto child() const noexcept -> kernel::mm::RegionKey {
        return child_;
    }
    [[nodiscard]] auto range() const noexcept -> kernel::mm::VirtRange {
        return range_;
    }

private:
    friend class kernel::mm::VSpace;
    friend class GrantGraph;

    constexpr RegionDerivation(
        kernel::mm::RegionKey parent,
        kernel::mm::RegionKey child,
        kernel::mm::VirtRange range) noexcept
        : parent_(parent), child_(child), range_(range) {}

    kernel::mm::RegionKey parent_{};
    kernel::mm::RegionKey child_{};
    kernel::mm::VirtRange range_{};
};

} // namespace kernel::cap
